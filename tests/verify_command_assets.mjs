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
  "cmake/command_assets.generated.cmake",
  "docs/man/tgcli.1",
  "docs/commands/public-command-registry.json",
  "docs/release/command-assets.json",
];
if (JSON.stringify(packageFiles) !== JSON.stringify(expectedPackageFiles)) {
  throw new Error("command asset package list differs");
}
const releasePackageAssets = run("node", [generator, "list-release-package-assets"])
  .trimEnd()
  .split("\n");
const expectedReleasePackageAssets = [
  "completions/tgcli.bash\tshare/bash-completion/completions/tgcli",
  "completions/tgcli.fish\tshare/fish/vendor_completions.d/tgcli.fish",
  "completions/_tgcli\tshare/zsh/site-functions/_tgcli",
  "docs/commands/public-command-registry.json\tshare/tgcli/public-command-registry.json",
  "docs/man/tgcli.1\tshare/man/man1/tgcli.1",
];
if (JSON.stringify(releasePackageAssets) !== JSON.stringify(expectedReleasePackageAssets)) {
  throw new Error("release command asset package manifest differs");
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
const active = [...leaves.values()].filter((leaf) => leaf.activation === "active");
const future = [...leaves.values()].filter((leaf) => leaf.activation === "future");
if (leaves.size !== 82 || active.length !== 81 || future.length !== 1) {
  throw new Error("command activation counts differ");
}
if (leaves.get("completion")?.activation !== "active") {
  throw new Error("completion activation differs");
}
if (leaves.get("raw")?.activation !== "future") {
  throw new Error("raw activation differs");
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
const completion = leaves.get("completion");
if (
  JSON.stringify([...registry.option_sets[completion.option_set], ...completion.options]) !==
  JSON.stringify(["--verbose", "-v", "--no-daemon", "--no-color"])
) {
  throw new Error("completion options differ");
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
if (!fishAsset.includes("function __tgcli_state")) {
  throw new Error("fish ordered token walker is missing");
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
const bashCase = (words, current) =>
  run("bash", [
    "-c",
    `source "$1"; COMP_WORDS=(${words.map((word) => `'${word}'`).join(" ")}); COMP_CWORD=${current}; _tgcli_complete; printf '%s\\n' "\${COMPREPLY[@]}"`,
    "bash",
    assets[0],
  ]).trim().split("\n").filter(Boolean);
const zshCase = (words, current) =>
  run("zsh", [
    "-f",
    "-c",
    `words=(${words.map((word) => `'${word}'`).join(" ")}); CURRENT=${current}; _describe() { print -rl -- \$candidates; }; source "$1"`,
    "zsh",
    assets[1],
  ]).trim().split("\n").filter(Boolean);

for (const shellCase of [bashCase, zshCase]) {
  const permissions = shellCase(
    ["tgcli", "--account", "main", "chat", "--json", "set-permissions", "--p"],
    6 + (shellCase === zshCase ? 1 : 0),
  );
  if (!permissions.includes("--permissions") || permissions.includes("--allow")) {
    throw new Error("interspersed grouped completion differs");
  }
  const group = shellCase(
    ["tgcli", "--json", "chat", "--account", "main", "se"],
    5 + (shellCase === zshCase ? 1 : 0),
  );
  if (
    !group.includes("set-title") ||
    (shellCase === bashCase && (group.includes("info") || group.includes("members"))) ||
    (shellCase === zshCase && (!group.includes("info") || !group.includes("members")))
  ) {
    throw new Error("active grouped child completion differs");
  }
  const info = shellCase(
    ["tgcli", "chat", "i"],
    2 + (shellCase === zshCase ? 1 : 0),
  );
  const members = shellCase(
    ["tgcli", "chat", "m"],
    2 + (shellCase === zshCase ? 1 : 0),
  );
  if (!info.includes("info") || !members.includes("members")) {
    throw new Error("new M2 grouped completion differs");
  }
  const pending = shellCase(
    ["tgcli", "chat", "--account", ""],
    3 + (shellCase === zshCase ? 1 : 0),
  );
  if (pending.length !== 0) {
    throw new Error("global option value was treated as a command token");
  }
  const topLevel = shellCase(
    ["tgcli", "d"],
    1 + (shellCase === zshCase ? 1 : 0),
  );
  if (!topLevel.includes("daemon") || !topLevel.includes("doctor") || !topLevel.includes("download")) {
    throw new Error("active download command is missing from completion");
  }
}

if (spawnSync("fish", ["--version"], { encoding: "utf8" }).status === 0) {
  run("fish", ["-n", assets[2]]);
  const state = run("fish", [
    "-c",
    `source "$argv[1]"; __tgcli_state tgcli --account main chat --json set-permissions`,
    assets[2],
  ]).trim();
  if (state !== "chat|set-permissions|0") {
    throw new Error(`fish ordered token state differs: ${state}`);
  }
}

const binary = process.argv[2];
if (binary) {
  const isolatedEnvironment = {
    ...process.env,
    HOME: "/tgcli-completion-missing-home",
    XDG_CONFIG_HOME: "/tgcli-completion-missing-config",
    XDG_RUNTIME_DIR: "/tgcli-completion-missing-runtime",
    TGCLI_ACCOUNT: "invalid account value",
    TGCLI_ALLOW_WRITE: "invalid",
    NO_COLOR: "1",
  };
  for (const [shell, asset] of [["bash", assets[0]], ["zsh", assets[1]], ["fish", assets[2]]]) {
    const expected = fs.readFileSync(asset);
    for (const extra of [[], ["--no-color"], ["--verbose"], ["--no-daemon"]]) {
      const outcome = spawnSync(binary, [...extra, "completion", shell], {
        env: isolatedEnvironment,
      });
      if (outcome.status !== 0 || outcome.stderr.length !== 0 || !outcome.stdout.equals(expected)) {
        throw new Error(`runtime completion bytes differ: ${shell} ${extra.join(" ")}`);
      }
    }
  }
  const rejected = [
    ["completion"],
    ["completion", "powershell"],
    ["completion", "bash", "extra"],
    ["--account", "main", "completion", "bash"],
    ["--json", "completion", "bash"],
    ["--full", "completion", "bash"],
    ["--allow-write", "completion", "bash"],
    ["--yes", "completion", "bash"],
    ["--dry-run", "completion", "bash"],
    ["--timeout", "1", "completion", "bash"],
    ["--cursor", "opaque", "completion", "bash"],
    ["--idempotency-key", "key", "completion", "bash"],
  ];
  for (const args of rejected) {
    const outcome = spawnSync(binary, args, { env: isolatedEnvironment });
    if (outcome.status !== 2 || outcome.stdout.length !== 0 || outcome.stderr.length === 0) {
      throw new Error(`completion rejection differs: ${args.join(" ")}`);
    }
  }

  const parsedHelpOptions = (help) => {
    const options = new Set();
    for (const line of help.split("\n")) {
      const match = line.match(/^\s+(?:(-[A-Za-z]),\s+)?(--[a-z][a-z0-9-]*|-[A-Za-z])(?:\s|$)/u);
      if (match) {
        if (match[1]) options.add(match[1]);
        options.add(match[2]);
      }
    }
    return options;
  };
  const globalOptions = new Set([
    ...registry.global_options.flags,
    ...registry.global_options.values,
  ]);
  const commandOptions = new Set(
    [...leaves.values()].flatMap((leaf) => leaf.options).filter((option) => !globalOptions.has(option)),
  );
  for (const [commandPath, leaf] of leaves) {
    if (leaf.activation !== "active") continue;
    const tokens = commandPath.split(" ");
    const args = tokens.length === 2
      ? ["--json", tokens[0], "--no-color", tokens[1], "--help"]
      : ["--json", tokens[0], "--no-color", "--help"];
    const outcome = spawnSync(binary, args, { encoding: "utf8" });
    if (outcome.status !== 0) {
      throw new Error(`parser rejected registry path ${commandPath}: ${outcome.stderr}`);
    }
    const help = `${outcome.stdout}${outcome.stderr}`;
    if (!help.includes("--no-color")) {
      throw new Error(`nested help hides --no-color: ${commandPath}`);
    }
    const actualOptions = parsedHelpOptions(help);
    for (const option of commandOptions) {
      if (actualOptions.has(option) !== leaf.options.includes(option)) {
        throw new Error(`parser/registry option mismatch for ${commandPath}: ${option}`);
      }
    }
    for (const positional of leaf.positionals) {
      const name = positional.replace(/[?.]+$/u, "").split("|")[0];
      if (name && !help.includes(name)) {
        throw new Error(`parser/registry positional mismatch for ${commandPath}: ${positional}`);
      }
    }
  }
}
