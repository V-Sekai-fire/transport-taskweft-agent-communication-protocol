# Copyright (c) 2026 K. S. Ernest (iFire) Lee
# SPDX-License-Identifier: MIT

defmodule Mix.Tasks.TaskweftAcp.Body do
  @shortdoc "Run a scan controller through the body host and log every tick as a session"

  @moduledoc """
  The body host, headless (RFD 2237): a controller in any form the compiler reads
  is lowered to its scan guest, Godot runs `body_host.gd` over a recorded trace (or
  live inputs when no trace is given), and every `tick` line the host prints is
  appended to a new session as an `acp/tick` event, so a run is replayable.

      mix taskweft_acp.body --controller walk_ctl.fbd --trace walk_trace.json

  Options: `--controller` (required), `--trace`, `--motion http://host:port` (the
  motion-bricks demo server), `--godot` (else `TASKWEFT_GODOT`), `--project` (the
  Godot project carrying the sandbox addon; default the sibling
  taskweft-godot-sandbox checkout), `--compiler` (else the sibling
  taskweft-fbd-compiler build). A controller the compiler refuses, a guest that
  does not compile, or a fault on any tick exits 1 with the reason.
  """

  use Mix.Task

  alias TaskweftAcp.Session
  alias TaskweftAcp.Session.Store

  @impl Mix.Task
  def run(argv) do
    Mix.Task.run("app.start")

    {opts, _, _} =
      OptionParser.parse(argv,
        strict: [
          controller: :string,
          trace: :string,
          motion: :string,
          godot: :string,
          project: :string,
          compiler: :string
        ]
      )

    controller = Keyword.get(opts, :controller) || Mix.raise("--controller is required")

    godot =
      Keyword.get(opts, :godot) || System.get_env("TASKWEFT_GODOT") ||
        Mix.raise("--godot or TASKWEFT_GODOT is required")

    project =
      Path.expand(
        Keyword.get(opts, :project) ||
          "../../3-interactor/taskweft-godot-sandbox/priv/godot_project"
      )

    compiler = Keyword.get(opts, :compiler) || find_compiler()

    guest = Path.join([project, "plans", "body.sgd"])
    File.mkdir_p!(Path.dirname(guest))

    case System.cmd(compiler, ["emit-scan", Path.expand(controller), guest],
           stderr_to_stdout: true
         ) do
      {_, 0} -> :ok
      {out, _} -> Mix.raise("the compiler refused #{controller}:\n#{out}")
    end

    host = Path.expand("priv/godot_project/body_host.gd", File.cwd!())

    args =
      [
        "--headless",
        "--path",
        project,
        "--script",
        host,
        "--",
        "--controller",
        "res://plans/body.sgd"
      ] ++
        opt_args(opts, :trace, "--trace", &Path.expand/1) ++
        opt_args(opts, :motion, "--motion", & &1)

    {out, _code} = System.cmd(godot, args, stderr_to_stdout: true)

    ticks =
      out
      |> String.split(~r/\r?\n/)
      |> Enum.filter(&String.starts_with?(&1, "tick "))
      |> Enum.map(fn "tick " <> json -> Jason.decode!(json) end)

    if ticks == [], do: Mix.raise("the host printed no tick line:\n#{out}")

    store = store()
    id = Session.new_id()
    Store.create_session(store, id, %{"cwd" => File.cwd!(), "controller" => controller})

    for t <- ticks do
      Store.append(store, id, Session.event(:agent_to_client, "acp/tick", t))
    end

    faults = Enum.filter(ticks, &Map.has_key?(&1["out"], "_fault"))
    last = List.last(ticks)

    IO.puts(
      Jason.encode!(%{
        "session" => id,
        "controller" => controller,
        "ticks" => length(ticks),
        "faults" => length(faults),
        "last_out" => last["out"]
      })
    )

    if faults != [], do: exit({:shutdown, 1})
  end

  # The application's store runs only under `serve`; a desk run keeps its sessions in
  # the plain helper store under `.taskweft-acp/sessions`.
  defp store do
    case Process.whereis(Store) do
      nil ->
        dir = Path.expand(".taskweft-acp/sessions", File.cwd!())
        File.mkdir_p!(dir)

        {:ok, pid} =
          Store.start_link(
            name: nil,
            adapter: TaskweftAcp.Session.Store.Vfs,
            adapter_opts: [dir: dir, mode: :plain]
          )

        IO.puts(:stderr, "sessions: #{dir}")
        pid

      pid ->
        pid
    end
  end

  defp opt_args(opts, key, flag, f) do
    case Keyword.get(opts, key) do
      nil -> []
      v -> [flag, f.(v)]
    end
  end

  defp find_compiler do
    base =
      Path.expand(
        "../../3-interactor/taskweft-fbd-compiler/.lake/build/bin/taskweft_fbd_compiler"
      )

    Enum.find([base <> ".exe", base], &File.exists?/1) ||
      Mix.raise("no taskweft-fbd-compiler build beside this checkout; pass --compiler")
  end
end
