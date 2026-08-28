#!/usr/bin/env node

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const repository = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const registryPath = path.join(repository, "docs", "commands", "public-command-registry.json");
const outputDirectory = path.join(repository, "completions");
const outputPaths = {
  bash: path.join(outputDirectory, "tgcli.bash"),
  zsh: path.join(outputDirectory, "_tgcli"),
  fish: path.join(outputDirectory, "tgcli.fish"),
};
const commandName = /^[a-z0-9][a-z0-9-]*$/;
const optionSetName = /^[a-z][a-z0-9_]*$/;
const optionName = /^(?:-[A-Za-z]|--[a-z0-9][a-z0-9-]*)$/;

const fail = (message) => {
  throw new Error(message);
};
const requireCondition = (condition, message) => {
  if (!condition) fail(message);
};
const sortedUnique = (values) =>
  values.length === new Set(values).size &&
  values.every((value, index) => index === 0 || values[index - 1] < value);
const quote = (value) => `'${value.replaceAll("'", "'\\''")}'`;

const registry = JSON.parse(fs.readFileSync(registryPath, "utf8"));
requireCondition(
  Object.keys(registry).sort().join(",") ===
    "commands,option_sets,schema_version,selection",
  "registry root keys differ",
);
requireCondition(registry.schema_version === 1, "registry version differs");
requireCondition(registry.selection === "raw-option-b", "registry selection differs");
requireCondition(Array.isArray(registry.commands), "registry commands must be an array");

const optionSets = new Map(Object.entries(registry.option_sets));
for (const [name, options] of optionSets) {
  requireCondition(optionSetName.test(name), `invalid option set name: ${name}`);
  requireCondition(Array.isArray(options), `invalid option set: ${name}`);
  requireCondition(options.length === new Set(options).size, `duplicate option in ${name}`);
  requireCondition(options.every((option) => optionName.test(option)), `invalid option in ${name}`);
  requireCondition(!options.includes("--full") && !options.includes("--bot-token"), `forbidden option in ${name}`);
}

const topNames = registry.commands.map((command) => command.name);
requireCondition(sortedUnique(topNames), "top-level commands are not sorted and unique");
const leaves = [];
for (const command of registry.commands) {
  requireCondition(commandName.test(command.name), `invalid command name: ${command.name}`);
  requireCondition(["active", "future"].includes(command.activation), `invalid activation: ${command.name}`);
  if (command.children) {
    requireCondition(
      Object.keys(command).sort().join(",") === "activation,children,name",
      `group keys differ: ${command.name}`,
    );
    const children = command.children.map((child) => child.name);
    requireCondition(sortedUnique(children), `children are not sorted: ${command.name}`);
    for (const child of command.children) {
      leaves.push({ ...child, path: `${command.name} ${child.name}`, top: command.name });
    }
  } else {
    leaves.push({ ...command, path: command.name, top: command.name });
  }
}

for (const leaf of leaves) {
  const keys = Object.keys(leaf)
    .filter((key) => !["path", "top"].includes(key))
    .sort()
    .join(",");
  requireCondition(
    keys === "activation,name,option_set,options" ||
      keys === "activation,alias_of,name,option_set,options" ||
      keys === "activation,name,option_set,options,positionals",
    `leaf keys differ: ${leaf.path}`,
  );
  requireCondition(["active", "future"].includes(leaf.activation), `invalid activation: ${leaf.path}`);
  requireCondition(optionSets.has(leaf.option_set), `unknown option set: ${leaf.path}`);
  requireCondition(Array.isArray(leaf.options), `invalid options: ${leaf.path}`);
  const options = [...optionSets.get(leaf.option_set), ...leaf.options];
  requireCondition(options.length === new Set(options).size, `duplicate effective option: ${leaf.path}`);
  requireCondition(options.every((option) => optionName.test(option)), `invalid option: ${leaf.path}`);
  requireCondition(!options.includes("--full") && !options.includes("--bot-token"), `forbidden option: ${leaf.path}`);
  leaf.effectiveOptions = options;
  leaf.positionals ??= [];
}

const raw = leaves.find((leaf) => leaf.path === "raw");
requireCondition(raw?.activation === "future", "raw must remain future");
requireCondition(raw.positionals.join(" ") === "-", "raw positional differs");
requireCondition(!raw.effectiveOptions.includes("--cursor"), "raw cursor leaked");
requireCondition(!raw.effectiveOptions.includes("--idempotency-key"), "raw idempotency leaked");
for (const future of ["completion", "download", "raw", "search", "chat info", "chat members"]) {
  requireCondition(leaves.find((leaf) => leaf.path === future)?.activation === "future", `future command activated: ${future}`);
}

const words = (values) => values.join(" ");
const candidates = (leaf) => words([...leaf.positionals, ...leaf.effectiveOptions]);
const casePattern = (value) => value.replaceAll(" ", "\\ ");
const childCases = registry.commands
  .filter((command) => command.children)
  .map((command) => `      ${command.name}) candidates=${quote(words(command.children.map((child) => child.name)))} ;;`)
  .join("\n");
const flatCases = leaves
  .filter((leaf) => !registry.commands.find((command) => command.name === leaf.top)?.children)
  .map((leaf) => `      ${leaf.path}) candidates=${quote(candidates(leaf))} ;;`)
  .join("\n");
const leafCases = leaves
  .map((leaf) => `      ${casePattern(leaf.path)}) candidates=${quote(candidates(leaf))} ;;`)
  .join("\n");

const bash = `# Generated from docs/commands/public-command-registry.json. Do not edit.\n_tgcli_complete() {\n  local current top key candidates candidate\n  current=\${COMP_WORDS[COMP_CWORD]}\n  top=\${COMP_WORDS[1]-}\n  candidates=''\n  if (( COMP_CWORD == 1 )); then\n    candidates=${quote(words(topNames))}\n  elif (( COMP_CWORD == 2 )); then\n    case \"$top\" in\n${childCases}\n${flatCases}\n    esac\n  else\n    key=\"$top \${COMP_WORDS[2]-}\"\n    case \"$key\" in\n${leafCases}\n    esac\n  fi\n  COMPREPLY=()\n  while IFS= read -r candidate; do\n    COMPREPLY[\${#COMPREPLY[@]}]=\"$candidate\"\n  done < <(compgen -W \"$candidates\" -- \"$current\")\n}\ncomplete -F _tgcli_complete tgcli\n`;

const zshTop = topNames.map(quote).join(" ");
const zshSecond = registry.commands
  .map((command) => {
    const values = command.children
      ? command.children.map((child) => child.name)
      : [...leaves.find((leaf) => leaf.path === command.name).positionals, ...leaves.find((leaf) => leaf.path === command.name).effectiveOptions];
    return `      ${command.name}) candidates=(${values.map(quote).join(" ")}) ;;`;
  })
  .join("\n");
const zshLeaves = leaves
  .map((leaf) => `      ${casePattern(leaf.path)}) candidates=(${[...leaf.positionals, ...leaf.effectiveOptions].map(quote).join(" ")}) ;;`)
  .join("\n");
const zsh = `#compdef tgcli\n# Generated from docs/commands/public-command-registry.json. Do not edit.\nlocal -a candidates\nlocal key\nif (( CURRENT == 2 )); then\n  candidates=(${zshTop})\nelif (( CURRENT == 3 )); then\n  case \"$words[2]\" in\n${zshSecond}\n  esac\nelse\n  key=\"$words[2] $words[3]\"\n  case \"$key\" in\n${zshLeaves}\n  esac\nfi\n_describe 'tgcli value' candidates\n`;

const fishLines = [
  "# Generated from docs/commands/public-command-registry.json. Do not edit.",
  `complete -c tgcli -f -n '__fish_use_subcommand' -a ${quote(words(topNames))}`,
];
for (const command of registry.commands) {
  if (command.children) {
    fishLines.push(
      `complete -c tgcli -f -n ${quote(`__fish_seen_subcommand_from ${command.name}`)} -a ${quote(words(command.children.map((child) => child.name)))}`,
    );
  }
}
for (const leaf of leaves) {
  const condition = registry.commands.find((command) => command.name === leaf.top)?.children
    ? `__fish_seen_subcommand_from ${leaf.top}; and __fish_seen_subcommand_from ${leaf.name}`
    : `__fish_seen_subcommand_from ${leaf.path}`;
  fishLines.push(
    `complete -c tgcli -f -n ${quote(condition)} -a ${quote(candidates(leaf))}`,
  );
}
const fish = `${fishLines.join("\n")}\n`;

const outputs = { bash, zsh, fish };
const command = process.argv[2];
if (command === "emit") {
  fs.mkdirSync(outputDirectory, { recursive: true });
  for (const [shell, data] of Object.entries(outputs)) {
    fs.writeFileSync(outputPaths[shell], data);
  }
} else if (command === "check") {
  for (const [shell, data] of Object.entries(outputs)) {
    requireCondition(fs.readFileSync(outputPaths[shell], "utf8") === data, `${shell} completion differs`);
  }
} else if (command === "list-package-files") {
  console.log("completions/_tgcli");
  console.log("completions/tgcli.bash");
  console.log("completions/tgcli.fish");
  console.log("docs/commands/public-command-registry.json");
} else {
  fail("expected emit, check, or list-package-files");
}
