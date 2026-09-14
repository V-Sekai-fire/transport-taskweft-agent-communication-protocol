# Copyright (c) 2026 K. S. Ernest (iFire) Lee
# SPDX-License-Identifier: MIT

defmodule Mix.Tasks.TaskweftAcp.PlanSgd do
  @shortdoc "Plan a todo against a domain headlessly and write the guest program"

  @moduledoc """
  The planner as a diagram source (RFD 2236): a domain file and a todo list in,
  the same SafeGDScript guest `/export` writes out, every step marked completed
  so the host performs all of it. taskweft-fbd-compiler lifts that guest into a
  Function Block Diagram.

      mix taskweft_acp.plan_sgd --domain priv/domains/repo_chores.ex \\
        --todo "tests_pass" --todo "git_commit fix" --out plan.sgd

  A todo is the task name followed by its arguments, space-separated. The plan
  is also printed as JSON on stdout so a writer can record the planner's steps
  as the row's provenance. A goal the planner cannot reach exits 1 with its
  reason.
  """

  use Mix.Task

  alias TaskweftAcp.{Domain, Export, Planner, Session}

  @impl Mix.Task
  def run(argv) do
    Mix.Task.run("app.start")

    {opts, _, _} =
      OptionParser.parse(argv,
        strict: [domain: :string, todo: :keep, out: :string, name: :string]
      )

    domain_path = Keyword.get(opts, :domain) || Domain.builtin_path()
    todo = opts |> Keyword.get_values(:todo) |> Enum.map(&String.split(&1, " ", trim: true))
    out = Keyword.get(opts, :out) || "plan.sgd"

    with {:ok, domain} <- Domain.load(File.read!(domain_path), domain_path),
         {:ok, plan} <- Planner.plan(domain, todo) do
      steps = plan["plan"] || []
      status = steps |> Enum.with_index() |> Map.new(fn {_, i} -> {i, "completed"} end)

      session = %{
        Session.new(Keyword.get(opts, :name, "planned"), File.cwd!())
        | domain: domain,
          todo: todo,
          plan: plan,
          executed_prefix: length(steps),
          step_status: status
      }

      File.write!(out, Export.sgd(session))
      IO.puts(Jason.encode!(%{"domain" => domain_path, "todo" => todo, "steps" => steps}))
    else
      {:error, reason} ->
        IO.puts(:stderr, "error: #{reason}")
        exit({:shutdown, 1})
    end
  end
end
