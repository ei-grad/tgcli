#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const repository = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const generator = path.join(repository, "scripts", "generate_command_assets.mjs");
const run = (command, args) => {
  const result = spawnSync(command, args, { cwd: repository, encoding: "utf8" });
  if (result.status !== 0) {
    throw new Error(`${command} failed: ${result.stderr}`);
  }
  return result.stdout;
};

run("node", [generator, "check"]);
const packageFiles = run("node", [generator, "list-package-files"])
  .trimEnd()
  .split("\n");
const expectedPackageFiles = [
  "completions/_tgcli",
  "completions/tgcli.bash",
  "completions/tgcli.fish",
  "docs/commands/public-command-registry.json",
];
if (JSON.stringify(packageFiles) !== JSON.stringify(expectedPackageFiles)) {
  throw new Error("command asset package list differs");
}

const registry = JSON.parse(
  fs.readFileSync(path.join(repository, "docs", "commands", "public-command-registry.json")),
);
const leaves = new Map();
for (const command of registry.commands) {
  if (command.children) {
    for (const child of command.children) {
      leaves.set(`${command.name} ${child.name}`, child);
    }
  } else {
    leaves.set(command.name, command);
  }
}
for (const future of ["chat info", "chat members", "completion", "download", "raw", "search"]) {
  if (leaves.get(future)?.activation !== "future") {
    throw new Error(`future command activation differs: ${future}`);
  }
}
const raw = leaves.get("raw");
const rawOptions = [
  ...registry.option_sets[raw.option_set],
  ...raw.options,
];
for (const forbidden of ["--cursor", "--idempotency-key", "--full", "--bot-token"]) {
  if (rawOptions.includes(forbidden)) {
    throw new Error(`raw completion exposes ${forbidden}`);
  }
}
if (raw.positionals.join(" ") !== "-") {
  throw new Error("raw completion positional differs");
}

const assets = [
  path.join(repository, "completions", "tgcli.bash"),
  path.join(repository, "completions", "_tgcli"),
  path.join(repository, "completions", "tgcli.fish"),
];
if (fs.readFileSync(assets[0], "utf8").includes("mapfile")) {
  throw new Error("bash completion requires a post-3.2 mapfile builtin");
}
const fishAsset = fs.readFileSync(assets[2], "utf8");
if (!fishAsset.includes("__fish_seen_subcommand_from chat; and __fish_seen_subcommand_from info")) {
  throw new Error("fish grouped leaf condition is not path-specific");
}
for (const asset of assets) {
  const bytes = fs.readFileSync(asset);
  const text = bytes.toString("utf8");
  if (text.includes("\r") || !text.endsWith("\n") || text.endsWith("\n\n")) {
    throw new Error(`noncanonical line endings: ${asset}`);
  }
  for (const executable of ["$(tgcli", "`tgcli", "eval "]) {
    if (text.includes(executable)) {
      throw new Error(`completion executes dynamic content: ${asset}`);
    }
  }
}

run("bash", ["-n", assets[0]]);
run("zsh", ["-n", assets[1]]);
if (spawnSync("fish", ["--version"], { encoding: "utf8" }).status === 0) {
  run("fish", ["-n", assets[2]]);
}
