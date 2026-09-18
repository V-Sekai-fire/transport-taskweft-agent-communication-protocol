// YCSB workload F driver: N workers, `usertable`, 50 % READ / 50 % RMW.
//
//   ycsb_direct <recordcount> <opcount> [workers] [batch]
//
// SPDX-License-Identifier: Apache-2.0
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

extern "C" {
int weft_fdb_start(const char *cluster_file);
void weft_fdb_stop(void);
int weft_vfs_register(int make_default);
}

namespace {

constexpr const char *kVfsName = "weft_fdb";
constexpr const char *kTableName = "usertable";

std::uint64_t now_us() {
    using clock = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(clock::now().time_since_epoch())
            .count());
}

std::uint64_t percentile(std::vector<std::uint64_t>& v, double p) {
    if (v.empty()) return 0;
    const std::size_t idx = static_cast<std::size_t>((p / 100.0) * (v.size() - 1));
    return v[idx];
}

void report(const char* label, std::vector<std::uint64_t>& lat) {
    if (lat.empty()) { std::printf("[%s] no samples\n", label); return; }
    std::sort(lat.begin(), lat.end());
    double sum = 0.0;
    for (auto x : lat) sum += static_cast<double>(x);
    std::printf("[%s] count=%zu avg_us=%.0f p50=%" PRIu64 " p95=%" PRIu64
                " p99=%" PRIu64 " max=%" PRIu64 "\n",
                label, lat.size(), sum / lat.size(),
                percentile(lat, 50.0), percentile(lat, 95.0),
                percentile(lat, 99.0), lat.back());
}

struct WorkerResult {
    std::vector<std::uint64_t> load_lat;
    std::vector<std::uint64_t> read_lat;
    std::vector<std::uint64_t> rmw_lat;
    bool ok = true;
};

std::string db_filename_for(unsigned worker_id) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "ycsb-%u.db", worker_id);
    return buf;
}

sqlite3 *open_db(const std::string &filename, const char *vfs) {
    sqlite3 *db = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE;
    if (sqlite3_open_v2(filename.c_str(), &db, flags, vfs) != SQLITE_OK) {
        std::fprintf(stderr, "open %s: %s\n", filename.c_str(),
                     db ? sqlite3_errmsg(db) : "(no handle)");
        if (db) sqlite3_close(db);
        return nullptr;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=MEMORY", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA locking_mode=EXCLUSIVE", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA cache_size=-4194304", nullptr, nullptr, nullptr);
    return db;
}

int create_usertable(sqlite3 *db) {
    const char *ddl =
        "CREATE TABLE IF NOT EXISTS usertable (YCSB_KEY VARCHAR(255) PRIMARY KEY, "
        "FIELD0 TEXT, FIELD1 TEXT, FIELD2 TEXT, FIELD3 TEXT, FIELD4 TEXT, "
        "FIELD5 TEXT, FIELD6 TEXT, FIELD7 TEXT, FIELD8 TEXT, FIELD9 TEXT)";
    return sqlite3_exec(db, ddl, nullptr, nullptr, nullptr);
}

bool load_phase(sqlite3 *db, std::uint64_t recordcount, std::uint64_t batch,
                WorkerResult &out) {
    sqlite3_stmt *ins = nullptr;
    if (sqlite3_prepare_v3(db,
        "INSERT INTO usertable VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)",
        -1, SQLITE_PREPARE_PERSISTENT, &ins, nullptr) != SQLITE_OK) return false;

    const std::string value(100, 'x');
    out.load_lat.reserve(recordcount / batch + 1);
    for (std::uint64_t base = 0; base < recordcount; base += batch) {
        const std::uint64_t hi = std::min(base + batch, recordcount);
        const std::uint64_t t = now_us();
        sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
        for (std::uint64_t i = base; i < hi; ++i) {
            char key[32]; std::snprintf(key, sizeof key, "user%" PRIu64, i);
            sqlite3_bind_text(ins, 1, key, -1, SQLITE_TRANSIENT);
            for (int f = 0; f < 10; ++f) {
                sqlite3_bind_text(ins, 2 + f, value.data(), (int)value.size(), SQLITE_STATIC);
            }
            if (sqlite3_step(ins) != SQLITE_DONE) {
                std::fprintf(stderr, "load step: %s\n", sqlite3_errmsg(db));
                sqlite3_finalize(ins); return false;
            }
            sqlite3_reset(ins);
        }
        sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
        out.load_lat.push_back(now_us() - t);
    }
    sqlite3_finalize(ins);
    return true;
}

bool run_phase(sqlite3 *db, std::uint32_t seed, std::uint64_t recordcount,
               std::uint64_t opcount, std::uint64_t batch, WorkerResult &out) {
    sqlite3_stmt *sel = nullptr, *upd = nullptr;
    if (sqlite3_prepare_v3(db, "SELECT * FROM usertable WHERE YCSB_KEY=?1",
                           -1, SQLITE_PREPARE_PERSISTENT, &sel, nullptr) != SQLITE_OK
        || sqlite3_prepare_v3(db, "UPDATE usertable SET FIELD0=?1 WHERE YCSB_KEY=?2",
                              -1, SQLITE_PREPARE_PERSISTENT, &upd, nullptr) != SQLITE_OK) {
        if (sel) sqlite3_finalize(sel);
        if (upd) sqlite3_finalize(upd);
        return false;
    }

    out.read_lat.reserve(opcount / 2 + 1);
    out.rmw_lat.reserve(opcount / 2 + 1);
    std::uint32_t rng = seed;
    sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
    std::uint64_t in_txn = 0;
    for (std::uint64_t i = 0; i < opcount; ++i) {
        rng = rng * 1103515245u + 12345u;
        const std::uint64_t k = static_cast<std::uint64_t>(rng) % recordcount;
        char key[32]; std::snprintf(key, sizeof key, "user%" PRIu64, k);
        if ((i & 1u) == 0u) {
            const std::uint64_t t = now_us();
            sqlite3_bind_text(sel, 1, key, -1, SQLITE_TRANSIENT);
            while (sqlite3_step(sel) == SQLITE_ROW) { /* drop rows */ }
            sqlite3_reset(sel);
            out.read_lat.push_back(now_us() - t);
        } else {
            const std::uint64_t t = now_us();
            char v[32]; std::snprintf(v, sizeof v, "v%" PRIu64, i);
            sqlite3_bind_text(upd, 1, v, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(upd, 2, key, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(upd) != SQLITE_DONE) {
                std::fprintf(stderr, "update: %s\n", sqlite3_errmsg(db));
                sqlite3_finalize(sel); sqlite3_finalize(upd); return false;
            }
            sqlite3_reset(upd);
            out.rmw_lat.push_back(now_us() - t);
            if (++in_txn >= batch) {
                sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
                sqlite3_exec(db, "BEGIN", nullptr, nullptr, nullptr);
                in_txn = 0;
            }
        }
    }
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
    sqlite3_finalize(sel);
    sqlite3_finalize(upd);
    return true;
}

void run_worker(unsigned worker_id, std::uint64_t recordcount, std::uint64_t opcount,
                std::uint64_t batch, WorkerResult &out) {
    const std::string filename = db_filename_for(worker_id);
    sqlite3 *db = open_db(filename, kVfsName);
    if (!db) { out.ok = false; return; }
    if (create_usertable(db) != SQLITE_OK) {
        std::fprintf(stderr, "worker %u create: %s\n", worker_id, sqlite3_errmsg(db));
        out.ok = false; sqlite3_close(db); return;
    }
    if (!load_phase(db, recordcount, batch, out)) { out.ok = false; sqlite3_close(db); return; }
    const std::uint32_t seed = 0x9e3779b9u ^ worker_id;
    if (!run_phase(db, seed, recordcount, opcount, batch, out)) out.ok = false;
    sqlite3_close(db);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: ycsb_direct <recordcount> <opcount> [workers] [batch]\n"
                             "\n"
                             "  YCSB workload F: 50%% READ + 50%% RMW on `usertable`.\n"
                             "  One SQLite database file per worker (roundtable shape).\n");
        return 2;
    }
    const std::uint64_t recordcount = std::strtoull(argv[1], nullptr, 10);
    const std::uint64_t opcount = std::strtoull(argv[2], nullptr, 10);
    const std::uint32_t workers = (argc > 3) ? (std::uint32_t)std::atoi(argv[3]) : 1u;
    const std::uint64_t batch = (argc > 4) ? std::strtoull(argv[4], nullptr, 10) : 1ULL;

    if (weft_fdb_start(std::getenv("WEFT_FDB_CLUSTER_FILE"))) {
        std::fprintf(stderr, "FoundationDB did not start\n");
        return 1;
    }
    weft_vfs_register(0);

    std::printf("== ycsb_direct: workers=%u recordcount=%" PRIu64 " opcount=%" PRIu64
                " batch=%" PRIu64 "\n",
                workers, recordcount, opcount, batch);

    std::vector<WorkerResult> results(workers);
    std::vector<std::thread> threads;
    threads.reserve(workers);
    const std::uint64_t wall_start = now_us();
    for (std::uint32_t w = 0; w < workers; ++w) {
        threads.emplace_back(run_worker, w, recordcount, opcount, batch, std::ref(results[w]));
    }
    for (auto& t : threads) t.join();
    const std::uint64_t wall_us = now_us() - wall_start;

    std::vector<std::uint64_t> all_read, all_rmw, all_load;
    std::uint64_t total_ops = 0;
    for (auto& r : results) {
        if (!r.ok) { weft_fdb_stop(); return 1; }
        total_ops += r.read_lat.size() + r.rmw_lat.size();
        all_read.insert(all_read.end(), r.read_lat.begin(), r.read_lat.end());
        all_rmw.insert(all_rmw.end(), r.rmw_lat.begin(), r.rmw_lat.end());
        all_load.insert(all_load.end(), r.load_lat.begin(), r.load_lat.end());
    }
    const double wall_sec = wall_us / 1e6;
    std::printf("[OVERALL] wall_s=%.3f workers=%u throughput=%.1f ops/sec\n",
                wall_sec, workers, static_cast<double>(total_ops) / wall_sec);
    report("READ", all_read);
    report("RMW", all_rmw);
    report("LOAD", all_load);

    weft_fdb_stop();
    return 0;
}
