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
const embeddedOutput = path.join(repository, "src", "cli", "completion_assets.generated.inc");
const releaseManifestOutput = path.join(repository, "docs", "release", "command-assets.json");
const cmakeInstallOutput = path.join(repository, "cmake", "command_assets.generated.cmake");
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

const releasePackageAssets = [
  {
    source: "completions/tgcli.bash",
    package: "share/bash-completion/completions/tgcli",
    shell: "bash",
  },
  {
    source: "completions/tgcli.fish",
    package: "share/fish/vendor_completions.d/tgcli.fish",
    shell: "fish",
  },
  {
    source: "completions/_tgcli",
    package: "share/zsh/site-functions/_tgcli",
    shell: "zsh",
  },
  {
    source: "docs/commands/public-command-registry.json",
    package: "share/tgcli/public-command-registry.json",
    shell: null,
  },
  {
    source: "docs/man/tgcli.1",
    package: "share/man/man1/tgcli.1",
    shell: null,
  },
];
const safeRelativeAsset = /^[A-Za-z0-9._/-]+$/;
for (const key of ["source", "package"]) {
  const values = releasePackageAssets.map((asset) => asset[key]);
  requireCondition(sortedUnique([...values].sort()), `release ${key} paths differ`);
  requireCondition(
    values.every(
      (value) =>
        safeRelativeAsset.test(value) &&
        !value.startsWith("/") &&
        !value.split("/").some((component) => ["", ".", ".."].includes(component)),
    ),
    `unsafe release ${key} path`,
  );
}
requireCondition(
  JSON.stringify(releasePackageAssets.filter((asset) => asset.shell).map((asset) => asset.shell).sort()) ===
    JSON.stringify(["bash", "fish", "zsh"]),
  "release completion shells differ",
);

const registry = JSON.parse(fs.readFileSync(registryPath, "utf8"));
requireCondition(
  Object.keys(registry).sort().join(",") ===
    "commands,global_options,option_sets,schema_version,selection",
  "registry root keys differ",
);
requireCondition(registry.schema_version === 1, "registry version differs");
requireCondition(registry.selection === "raw-option-b", "registry selection differs");
requireCondition(Array.isArray(registry.commands), "registry commands must be an array");
requireCondition(
  Object.keys(registry.global_options).sort().join(",") === "flags,values",
  "global option classes differ",
);
requireCondition(
  registry.global_options.flags.every((option) => optionName.test(option)) &&
    registry.global_options.values.every((option) => optionName.test(option)),
  "invalid global option",
);

const optionSets = new Map(Object.entries(registry.option_sets));
for (const [name, options] of optionSets) {
  requireCondition(optionSetName.test(name), `invalid option set name: ${name}`);
  requireCondition(Array.isArray(options), `invalid option set: ${name}`);
  requireCondition(options.length === new Set(options).size, `duplicate option in ${name}`);
  requireCondition(options.every((option) => optionName.test(option)), `invalid option in ${name}`);
  requireCondition(!options.includes("--full") && !options.includes("--bot-token"), `forbidden option in ${name}`);
}

const allTopNames = registry.commands.map((command) => command.name);
requireCondition(sortedUnique(allTopNames), "top-level commands are not sorted and unique");
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
    keys === "activation,name,option_set,options,positionals" ||
      keys === "activation,alias_of,name,option_set,options,positionals" ||
      keys === "activation,completion_literals,name,option_set,options,positionals",
    `leaf keys differ: ${leaf.path}`,
  );
  requireCondition(["active", "future"].includes(leaf.activation), `invalid activation: ${leaf.path}`);
  requireCondition(optionSets.has(leaf.option_set), `unknown option set: ${leaf.path}`);
  requireCondition(Array.isArray(leaf.options), `invalid options: ${leaf.path}`);
  requireCondition(
    Array.isArray(leaf.positionals) &&
      leaf.positionals.every((value) => typeof value === "string" && value.length > 0),
    `invalid positionals: ${leaf.path}`,
  );
  const options = [...optionSets.get(leaf.option_set), ...leaf.options];
  requireCondition(options.length === new Set(options).size, `duplicate effective option: ${leaf.path}`);
  requireCondition(options.every((option) => optionName.test(option)), `invalid option: ${leaf.path}`);
  requireCondition(!options.includes("--full") && !options.includes("--bot-token"), `forbidden option: ${leaf.path}`);
  leaf.effectiveOptions = options;
  leaf.completion_literals ??= [];
  requireCondition(
    leaf.completion_literals.every((value) => typeof value === "string" && value.length > 0),
    `invalid completion literals: ${leaf.path}`,
  );
}

const raw = leaves.find((leaf) => leaf.path === "raw");
requireCondition(raw?.activation === "future", "raw must remain future");
requireCondition(raw.positionals.join(" ") === "-", "raw positional differs");
requireCondition(raw.completion_literals.join(" ") === "-", "raw completion literal differs");
requireCondition(!raw.effectiveOptions.includes("--cursor"), "raw cursor leaked");
requireCondition(!raw.effectiveOptions.includes("--idempotency-key"), "raw idempotency leaked");
requireCondition(
  leaves.find((leaf) => leaf.path === "completion")?.activation === "active",
  "completion must be active",
);
requireCondition(leaves.find((leaf) => leaf.path === "raw")?.activation === "future", "raw must remain future");

const visibleLeaves = leaves.filter((leaf) => leaf.activation === "active");
const visibleCommands = registry.commands
  .map((command) => ({
    ...command,
    children: command.children?.filter((child) => child.activation === "active"),
  }))
  .filter(
    (command) =>
      command.activation === "active" || command.children?.length,
  );
const visibleTopNames = visibleCommands.map((command) => command.name);
const topNames = visibleTopNames;
const words = (values) => values.join(" ");
const candidates = (leaf) => words([...leaf.completion_literals, ...leaf.effectiveOptions]);
const casePattern = (value) => value.replaceAll(" ", "\\ ");
const childCases = visibleCommands
  .filter((command) => command.children?.length)
  .map((command) => `      ${command.name}) candidates=${quote(words(command.children.map((child) => child.name)))} ;;`)
  .join("\n");
const flatCases = visibleLeaves
  .filter((leaf) => !registry.commands.find((command) => command.name === leaf.top)?.children)
  .map((leaf) => `      ${leaf.path}) candidates=${quote(candidates(leaf))} ;;`)
  .join("\n");
const leafCases = visibleLeaves
  .map((leaf) => `      ${casePattern(leaf.path)}) candidates=${quote(candidates(leaf))} ;;`)
  .join("\n");
const topScanPattern = visibleTopNames.join("|");
const childScanPattern = visibleCommands
  .filter((command) => command.children?.length)
  .flatMap((command) => command.children.map((child) => `${command.name}:${child.name}`))
  .join("|");
const globalValuePattern = registry.global_options.values.join("|");
const globalInlineOrFlagPattern = [
  ...registry.global_options.values.map((option) => `${option}=*`),
  ...registry.global_options.flags,
].join("|");

const bash = `# Generated from docs/commands/public-command-registry.json. Do not edit.\n_tgcli_complete() {\n  local current top key candidates candidate\n  current=\${COMP_WORDS[COMP_CWORD]}\n  top=\${COMP_WORDS[1]-}\n  candidates=''\n  if (( COMP_CWORD == 1 )); then\n    candidates=${quote(words(topNames))}\n  elif (( COMP_CWORD == 2 )); then\n    case \"$top\" in\n${childCases}\n${flatCases}\n    esac\n  else\n    key=\"$top \${COMP_WORDS[2]-}\"\n    case \"$key\" in\n${leafCases}\n    esac\n  fi\n  COMPREPLY=()\n  while IFS= read -r candidate; do\n    COMPREPLY[\${#COMPREPLY[@]}]=\"$candidate\"\n  done < <(compgen -W \"$candidates\" -- \"$current\")\n}\ncomplete -F _tgcli_complete tgcli\n`;

const zshTop = topNames.map(quote).join(" ");
const zshSecond = visibleCommands
  .map((command) => {
    const values = command.children
      ? command.children.map((child) => child.name)
      : [...visibleLeaves.find((leaf) => leaf.path === command.name).completion_literals, ...visibleLeaves.find((leaf) => leaf.path === command.name).effectiveOptions];
    return `      ${command.name}) candidates=(${values.map(quote).join(" ")}) ;;`;
  })
  .join("\n");
const zshLeaves = visibleLeaves
  .map((leaf) => `      ${casePattern(leaf.path)}) candidates=(${[...leaf.completion_literals, ...leaf.effectiveOptions].map(quote).join(" ")}) ;;`)
  .join("\n");
const zsh = `#compdef tgcli\n# Generated from docs/commands/public-command-registry.json. Do not edit.\nlocal -a candidates\nlocal key\nif (( CURRENT == 2 )); then\n  candidates=(${zshTop})\nelif (( CURRENT == 3 )); then\n  case \"$words[2]\" in\n${zshSecond}\n  esac\nelse\n  key=\"$words[2] $words[3]\"\n  case \"$key\" in\n${zshLeaves}\n  esac\nfi\n_describe 'tgcli value' candidates\n`;

const fishLines = [
  "# Generated from docs/commands/public-command-registry.json. Do not edit.",
  `complete -c tgcli -f -n '__fish_use_subcommand' -a ${quote(words(topNames))}`,
];
for (const command of visibleCommands) {
  if (command.children) {
    fishLines.push(
      `complete -c tgcli -f -n ${quote(`__fish_seen_subcommand_from ${command.name}`)} -a ${quote(words(command.children.map((child) => child.name)))}`,
    );
  }
}
for (const leaf of visibleLeaves) {
  const condition = registry.commands.find((command) => command.name === leaf.top)?.children
    ? `__fish_seen_subcommand_from ${leaf.top}; and __fish_seen_subcommand_from ${leaf.name}`
    : `__fish_seen_subcommand_from ${leaf.path}`;
  fishLines.push(
    `complete -c tgcli -f -n ${quote(condition)} -a ${quote(candidates(leaf))}`,
  );
}
const fish = `${fishLines.join("\n")}\n`;

const bashNormalize = `_tgcli_normalize() {\n  local token index pending top child current\n  pending=0\n  top=''\n  child=''\n  current=\${COMP_WORDS[COMP_CWORD]}\n  index=1\n  while (( index < COMP_CWORD )); do\n    token=\${COMP_WORDS[index]}\n    if (( pending )); then\n      pending=0\n    else\n      case \"$token\" in\n        ${globalValuePattern}) pending=1 ;;\n        ${globalInlineOrFlagPattern}|-*) ;;\n        *)\n          if [[ -z \"$top\" ]]; then\n            case \"$token\" in ${topScanPattern}) top=\"$token\" ;; esac\n          elif [[ -z \"$child\" ]]; then\n            case \"$top:$token\" in ${childScanPattern}) child=\"$token\" ;; esac\n          fi\n          ;;\n      esac\n    fi\n    ((index += 1))\n  done\n  if (( pending )); then\n    COMP_WORDS=(tgcli __tgcli_value_pending__ \"$current\")\n    COMP_CWORD=2\n  elif [[ -z \"$top\" ]]; then\n    COMP_WORDS=(tgcli \"$current\")\n    COMP_CWORD=1\n  elif [[ -z \"$child\" ]]; then\n    COMP_WORDS=(tgcli \"$top\" \"$current\")\n    COMP_CWORD=2\n  else\n    COMP_WORDS=(tgcli \"$top\" \"$child\" \"$current\")\n    COMP_CWORD=3\n  fi\n}`;
const generatedBash = bash.replace(
  "_tgcli_complete() {",
  `${bashNormalize}\n_tgcli_complete() {\n  _tgcli_normalize`,
);

const zshNormalize = `_tgcli_zsh_normalize() {\n  local token top child current\n  local -i index pending\n  pending=0\n  top=''\n  child=''\n  current=$words[CURRENT]\n  index=2\n  while (( index < CURRENT )); do\n    token=$words[index]\n    if (( pending )); then\n      pending=0\n    else\n      case \"$token\" in\n        ${globalValuePattern}) pending=1 ;;\n        ${globalInlineOrFlagPattern}|-*) ;;\n        *)\n          if [[ -z \"$top\" ]]; then\n            case \"$token\" in ${topScanPattern}) top=\"$token\" ;; esac\n          elif [[ -z \"$child\" ]]; then\n            case \"$top:$token\" in ${childScanPattern}) child=\"$token\" ;; esac\n          fi\n          ;;\n      esac\n    fi\n    ((index += 1))\n  done\n  if (( pending )); then\n    words=(tgcli __tgcli_value_pending__ \"$current\")\n    CURRENT=3\n  elif [[ -z \"$top\" ]]; then\n    words=(tgcli \"$current\")\n    CURRENT=2\n  elif [[ -z \"$child\" ]]; then\n    words=(tgcli \"$top\" \"$current\")\n    CURRENT=3\n  else\n    words=(tgcli \"$top\" \"$child\" \"$current\")\n    CURRENT=4\n  fi\n}`;
const generatedZsh = zsh.replace(
  "local -a candidates",
  `${zshNormalize}\n_tgcli_zsh_normalize\nlocal -a candidates`,
);

const fishWalkerLines = [
  "# Generated from docs/commands/public-command-registry.json. Do not edit.",
  "function __tgcli_state",
  "  set -l top ''",
  "  set -l child ''",
  "  set -l pending 0",
  "  set -l index 2",
  "  while test $index -le (count $argv)",
  "    set -l token $argv[$index]",
  "    if test $pending -eq 1",
  "      set pending 0",
  "    else",
  "      switch $token",
  `        case ${registry.global_options.values.map(quote).join(" ")}`,
  "          set pending 1",
  `        case ${[...registry.global_options.values.map((option) => `${option}=*`), ...registry.global_options.flags].map(quote).join(" ")} '-*'`,
  "        case '*'",
  "          if test -z \"$top\"",
  "            switch $token",
  `              case ${visibleTopNames.map(quote).join(" ")}`,
  "                set top $token",
  "            end",
  "          else if test -z \"$child\"",
  "            switch \"$top:$token\"",
  `              case ${visibleCommands
    .filter((command) => command.children?.length)
    .flatMap((command) => command.children.map((child) => quote(`${command.name}:${child.name}`)))
    .join(" ")}`,
  "                set child $token",
  "            end",
  "          end",
  "      end",
  "    end",
  "    set index (math $index + 1)",
  "  end",
  "  echo \"$top|$child|$pending\"",
  "end",
  `complete -c tgcli -f -n ${quote("test (__tgcli_state (commandline -opc)) = '||0'")} -a ${quote(words(visibleTopNames))}`,
];
for (const commandEntry of visibleCommands.filter((entry) => entry.children?.length)) {
  fishWalkerLines.push(
    `complete -c tgcli -f -n ${quote(`test (__tgcli_state (commandline -opc)) = '${commandEntry.name}||0'`)} -a ${quote(words(commandEntry.children.map((child) => child.name)))}`,
  );
}
for (const leaf of visibleLeaves) {
  const grouped = registry.commands.find((entry) => entry.name === leaf.top)?.children;
  const stateValue = grouped ? `${leaf.top}|${leaf.name}|0` : `${leaf.top}||0`;
  fishWalkerLines.push(
    `complete -c tgcli -f -n ${quote(`test (__tgcli_state (commandline -opc)) = '${stateValue}'`)} -a ${quote(candidates(leaf))}`,
  );
}
const generatedFish = `${fishWalkerLines.join("\n")}\n`;

const outputs = { bash: generatedBash, zsh: generatedZsh, fish: generatedFish };
const embeddedLiteral = (name, delimiter, data) => {
  requireCondition(!data.includes(`)${delimiter}\"`), `unsafe C++ delimiter: ${name}`);
  return `inline constexpr std::string_view ${name} = R\"${delimiter}(${data})${delimiter}\";`;
};
const embedded = [
  "// Generated from docs/commands/public-command-registry.json. Do not edit.",
  embeddedLiteral("kBashCompletion", "TGBASH", generatedBash),
  embeddedLiteral("kZshCompletion", "TGZSH", generatedZsh),
  embeddedLiteral("kFishCompletion", "TGFISH", generatedFish),
  "",
].join("\n");
const releaseManifest = `${JSON.stringify(
  { schema_version: 1, assets: releasePackageAssets },
  null,
  2,
)}\n`;
const cmakeInstall = [
  "# Generated from scripts/generate_command_assets.mjs. Do not edit.",
  ...releasePackageAssets.flatMap((asset) => {
    const destination = path.posix.dirname(asset.package);
    const filename = path.posix.basename(asset.package);
    return [
      `install(FILES "\${CMAKE_CURRENT_SOURCE_DIR}/${asset.source}"`,
      `    DESTINATION "${destination}"`,
      `    RENAME "${filename}")`,
    ];
  }),
  "",
].join("\n");
const command = process.argv[2];
if (command === "emit") {
  fs.mkdirSync(outputDirectory, { recursive: true });
  fs.mkdirSync(path.dirname(releaseManifestOutput), { recursive: true });
  fs.mkdirSync(path.dirname(cmakeInstallOutput), { recursive: true });
  for (const [shell, data] of Object.entries(outputs)) {
    fs.writeFileSync(outputPaths[shell], data);
  }
  fs.writeFileSync(embeddedOutput, embedded);
  fs.writeFileSync(releaseManifestOutput, releaseManifest);
  fs.writeFileSync(cmakeInstallOutput, cmakeInstall);
} else if (command === "check") {
  for (const [shell, data] of Object.entries(outputs)) {
    requireCondition(fs.readFileSync(outputPaths[shell], "utf8") === data, `${shell} completion differs`);
  }
  requireCondition(fs.readFileSync(embeddedOutput, "utf8") === embedded, "embedded completion differs");
  requireCondition(fs.readFileSync(releaseManifestOutput, "utf8") === releaseManifest, "release command asset manifest differs");
  requireCondition(fs.readFileSync(cmakeInstallOutput, "utf8") === cmakeInstall, "CMake command asset install differs");
} else if (command === "list-package-files") {
  console.log("completions/_tgcli");
  console.log("completions/tgcli.bash");
  console.log("completions/tgcli.fish");
  console.log("cmake/command_assets.generated.cmake");
  console.log("docs/man/tgcli.1");
  console.log("docs/commands/public-command-registry.json");
  console.log("docs/release/command-assets.json");
} else if (command === "list-release-package-assets") {
  for (const asset of releasePackageAssets) {
    console.log(`${asset.source}\t${asset.package}`);
  }
} else {
  fail("expected emit, check, list-package-files, or list-release-package-assets");
}
