// Generated from docs/commands/public-command-registry.json. Do not edit.
inline constexpr std::string_view kBashCompletion = R"TGBASH(# Generated from docs/commands/public-command-registry.json. Do not edit.
_tgcli_normalize() {
  local token index pending top child current
  pending=0
  top=''
  child=''
  current=${COMP_WORDS[COMP_CWORD]}
  index=1
  while (( index < COMP_CWORD )); do
    token=${COMP_WORDS[index]}
    if (( pending )); then
      pending=0
    else
      case "$token" in
        --account|--timeout|--cursor|--idempotency-key) pending=1 ;;
        --account=*|--timeout=*|--cursor=*|--idempotency-key=*|--json|--allow-write|--yes|--dry-run|--verbose|-v|--no-daemon|--no-color|-*) ;;
        *)
          if [[ -z "$top" ]]; then
            case "$token" in account|chat|chats|completion|contact|daemon|doctor|download|fetch|folder|history|listen|login|logout|me|msg|read|resolve|saved|schema|search|send|session|storage|topic|unread|version|wait-for) top="$token" ;; esac
          elif [[ -z "$child" ]]; then
            case "$top:$token" in account:add|account:list|account:remove|account:show|account:use|chat:archive|chat:ban|chat:demote|chat:info|chat:invite-link|chat:join|chat:kick|chat:leave|chat:mark-read|chat:members|chat:mute|chat:pin|chat:promote|chat:set-description|chat:set-permissions|chat:set-photo|chat:set-title|chat:unarchive|chat:unban|chat:unmute|chat:unpin|contact:add|contact:block|contact:list|contact:remove|contact:search|contact:unblock|daemon:restart|daemon:run|daemon:status|daemon:stop|folder:add-chat|folder:create|folder:delete|folder:edit|folder:list|folder:remove-chat|folder:show|msg:delete|msg:edit|msg:forward|msg:get|msg:link|msg:pin|msg:react|msg:unpin|saved:attach|saved:search|saved:tags|session:list|session:terminate|storage:optimize|storage:stats|topic:close|topic:create|topic:edit|topic:list|topic:reopen) child="$token" ;; esac
          fi
          ;;
      esac
    fi
    ((index += 1))
  done
  if (( pending )); then
    COMP_WORDS=(tgcli __tgcli_value_pending__ "$current")
    COMP_CWORD=2
  elif [[ -z "$top" ]]; then
    COMP_WORDS=(tgcli "$current")
    COMP_CWORD=1
  elif [[ -z "$child" ]]; then
    COMP_WORDS=(tgcli "$top" "$current")
    COMP_CWORD=2
  else
    COMP_WORDS=(tgcli "$top" "$child" "$current")
    COMP_CWORD=3
  fi
}
_tgcli_complete() {
  _tgcli_normalize
  local current top key candidates candidate
  current=${COMP_WORDS[COMP_CWORD]}
  top=${COMP_WORDS[1]-}
  candidates=''
  if (( COMP_CWORD == 1 )); then
    candidates='account chat chats completion contact daemon doctor download fetch folder history listen login logout me msg read resolve saved schema search send session storage topic unread version wait-for'
  elif (( COMP_CWORD == 2 )); then
    case "$top" in
      account) candidates='add list remove show use' ;;
      chat) candidates='archive ban demote info invite-link join kick leave mark-read members mute pin promote set-description set-permissions set-photo set-title unarchive unban unmute unpin' ;;
      contact) candidates='add block list remove search unblock' ;;
      daemon) candidates='restart run status stop' ;;
      folder) candidates='add-chat create delete edit list remove-chat show' ;;
      msg) candidates='delete edit forward get link pin react unpin' ;;
      saved) candidates='attach search tags' ;;
      session) candidates='list terminate' ;;
      storage) candidates='optimize stats' ;;
      topic) candidates='close create edit list reopen' ;;
      chats) candidates='--account --json --timeout --cursor --verbose -v --no-daemon --no-color --folder --archived --unread -n' ;;
      completion) candidates='bash zsh fish --verbose -v --no-daemon --no-color' ;;
      doctor) candidates='--json --verbose -v --no-daemon --no-color' ;;
      download) candidates='--account --json --timeout --verbose -v --no-daemon --no-color -O' ;;
      fetch) candidates='--account --json --timeout --verbose -v --no-daemon --no-color --limit --all --since' ;;
      history) candidates='--account --json --timeout --cursor --verbose -v --no-daemon --no-color -n --before --since --until --topic --local' ;;
      listen) candidates='--account --json --timeout --verbose -v --no-daemon --no-color --chat --types --count' ;;
      login) candidates='--account --json --timeout --verbose -v --no-daemon --no-color --qr --bot' ;;
      logout) candidates='--account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color' ;;
      me) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      read) candidates='--account --json --timeout --cursor --verbose -v --no-daemon --no-color -n --before --since --until --topic --local' ;;
      resolve) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      schema) candidates='--json --verbose -v --no-daemon --no-color --all' ;;
      search) candidates='--account --json --timeout --cursor --verbose -v --no-daemon --no-color --chat --global --from --type -n' ;;
      send) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --md --html --reply-to --topic --silent --schedule' ;;
      unread) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      version) candidates='--json --verbose -v --no-daemon --no-color' ;;
      wait-for) candidates='--account --json --timeout --verbose -v --no-daemon --no-color --chat --from --regex --after' ;;
    esac
  else
    key="$top ${COMP_WORDS[2]-}"
    case "$key" in
      account\ add) candidates='--json --verbose -v --no-color' ;;
      account\ list) candidates='--json --verbose -v --no-color' ;;
      account\ remove) candidates='--json --verbose -v --no-color --keep-session --reassign-default --yes' ;;
      account\ show) candidates='--json --verbose -v --no-color' ;;
      account\ use) candidates='--json --verbose -v --no-color' ;;
      chat\ archive) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ ban) candidates='--account --json --allow-write --dry-run --yes --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ demote) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ info) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      chat\ invite-link) candidates='--account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color --revoke' ;;
      chat\ join) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ kick) candidates='--account --json --allow-write --dry-run --yes --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ leave) candidates='--account --json --allow-write --dry-run --yes --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ mark-read) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ members) candidates='--account --json --timeout --cursor --verbose -v --no-daemon --no-color --admins --bots --query -n' ;;
      chat\ mute) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --for' ;;
      chat\ pin) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ promote) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --rights' ;;
      chat\ set-description) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ set-permissions) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --permissions' ;;
      chat\ set-photo) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --delete' ;;
      chat\ set-title) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ unarchive) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ unban) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ unmute) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chat\ unpin) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      chats) candidates='--account --json --timeout --cursor --verbose -v --no-daemon --no-color --folder --archived --unread -n' ;;
      completion) candidates='bash zsh fish --verbose -v --no-daemon --no-color' ;;
      contact\ add) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      contact\ block) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      contact\ list) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      contact\ remove) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      contact\ search) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      contact\ unblock) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      daemon\ restart) candidates='--json --verbose -v --no-color' ;;
      daemon\ run) candidates='--json --verbose -v --no-color' ;;
      daemon\ status) candidates='--json --verbose -v --no-color' ;;
      daemon\ stop) candidates='--json --verbose -v --no-color' ;;
      doctor) candidates='--json --verbose -v --no-daemon --no-color' ;;
      download) candidates='--account --json --timeout --verbose -v --no-daemon --no-color -O' ;;
      fetch) candidates='--account --json --timeout --verbose -v --no-daemon --no-color --limit --all --since' ;;
      folder\ add-chat) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      folder\ create) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --chat --icon --color' ;;
      folder\ delete) candidates='--account --json --allow-write --dry-run --yes --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      folder\ edit) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --name --icon --color' ;;
      folder\ list) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      folder\ remove-chat) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      folder\ show) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      history) candidates='--account --json --timeout --cursor --verbose -v --no-daemon --no-color -n --before --since --until --topic --local' ;;
      listen) candidates='--account --json --timeout --verbose -v --no-daemon --no-color --chat --types --count' ;;
      login) candidates='--account --json --timeout --verbose -v --no-daemon --no-color --qr --bot' ;;
      logout) candidates='--account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color' ;;
      me) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      msg\ delete) candidates='--account --json --allow-write --dry-run --yes --timeout --idempotency-key --verbose -v --no-daemon --no-color --for-all' ;;
      msg\ edit) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      msg\ forward) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --drop-author' ;;
      msg\ get) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      msg\ link) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      msg\ pin) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      msg\ react) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --remove --big' ;;
      msg\ unpin) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      read) candidates='--account --json --timeout --cursor --verbose -v --no-daemon --no-color -n --before --since --until --topic --local' ;;
      resolve) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      saved\ attach) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --caption' ;;
      saved\ search) candidates='--account --json --timeout --cursor --verbose -v --no-daemon --no-color --tag -n' ;;
      saved\ tags) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      schema) candidates='--json --verbose -v --no-daemon --no-color --all' ;;
      search) candidates='--account --json --timeout --cursor --verbose -v --no-daemon --no-color --chat --global --from --type -n' ;;
      send) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --md --html --reply-to --topic --silent --schedule' ;;
      session\ list) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      session\ terminate) candidates='--account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color' ;;
      storage\ optimize) candidates='--account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color' ;;
      storage\ stats) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      topic\ close) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      topic\ create) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --icon' ;;
      topic\ edit) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      topic\ list) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      topic\ reopen) candidates='--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color' ;;
      unread) candidates='--account --json --timeout --verbose -v --no-daemon --no-color' ;;
      version) candidates='--json --verbose -v --no-daemon --no-color' ;;
      wait-for) candidates='--account --json --timeout --verbose -v --no-daemon --no-color --chat --from --regex --after' ;;
    esac
  fi
  COMPREPLY=()
  while IFS= read -r candidate; do
    COMPREPLY[${#COMPREPLY[@]}]="$candidate"
  done < <(compgen -W "$candidates" -- "$current")
}
complete -F _tgcli_complete tgcli
)TGBASH";
inline constexpr std::string_view kZshCompletion = R"TGZSH(#compdef tgcli
# Generated from docs/commands/public-command-registry.json. Do not edit.
_tgcli_zsh_normalize() {
  local token top child current
  local -i index pending
  pending=0
  top=''
  child=''
  current=$words[CURRENT]
  index=2
  while (( index < CURRENT )); do
    token=$words[index]
    if (( pending )); then
      pending=0
    else
      case "$token" in
        --account|--timeout|--cursor|--idempotency-key) pending=1 ;;
        --account=*|--timeout=*|--cursor=*|--idempotency-key=*|--json|--allow-write|--yes|--dry-run|--verbose|-v|--no-daemon|--no-color|-*) ;;
        *)
          if [[ -z "$top" ]]; then
            case "$token" in account|chat|chats|completion|contact|daemon|doctor|download|fetch|folder|history|listen|login|logout|me|msg|read|resolve|saved|schema|search|send|session|storage|topic|unread|version|wait-for) top="$token" ;; esac
          elif [[ -z "$child" ]]; then
            case "$top:$token" in account:add|account:list|account:remove|account:show|account:use|chat:archive|chat:ban|chat:demote|chat:info|chat:invite-link|chat:join|chat:kick|chat:leave|chat:mark-read|chat:members|chat:mute|chat:pin|chat:promote|chat:set-description|chat:set-permissions|chat:set-photo|chat:set-title|chat:unarchive|chat:unban|chat:unmute|chat:unpin|contact:add|contact:block|contact:list|contact:remove|contact:search|contact:unblock|daemon:restart|daemon:run|daemon:status|daemon:stop|folder:add-chat|folder:create|folder:delete|folder:edit|folder:list|folder:remove-chat|folder:show|msg:delete|msg:edit|msg:forward|msg:get|msg:link|msg:pin|msg:react|msg:unpin|saved:attach|saved:search|saved:tags|session:list|session:terminate|storage:optimize|storage:stats|topic:close|topic:create|topic:edit|topic:list|topic:reopen) child="$token" ;; esac
          fi
          ;;
      esac
    fi
    ((index += 1))
  done
  if (( pending )); then
    words=(tgcli __tgcli_value_pending__ "$current")
    CURRENT=3
  elif [[ -z "$top" ]]; then
    words=(tgcli "$current")
    CURRENT=2
  elif [[ -z "$child" ]]; then
    words=(tgcli "$top" "$current")
    CURRENT=3
  else
    words=(tgcli "$top" "$child" "$current")
    CURRENT=4
  fi
}
_tgcli_zsh_normalize
local -a candidates
local key
if (( CURRENT == 2 )); then
  candidates=('account' 'chat' 'chats' 'completion' 'contact' 'daemon' 'doctor' 'download' 'fetch' 'folder' 'history' 'listen' 'login' 'logout' 'me' 'msg' 'read' 'resolve' 'saved' 'schema' 'search' 'send' 'session' 'storage' 'topic' 'unread' 'version' 'wait-for')
elif (( CURRENT == 3 )); then
  case "$words[2]" in
      account) candidates=('add' 'list' 'remove' 'show' 'use') ;;
      chat) candidates=('archive' 'ban' 'demote' 'info' 'invite-link' 'join' 'kick' 'leave' 'mark-read' 'members' 'mute' 'pin' 'promote' 'set-description' 'set-permissions' 'set-photo' 'set-title' 'unarchive' 'unban' 'unmute' 'unpin') ;;
      chats) candidates=('--account' '--json' '--timeout' '--cursor' '--verbose' '-v' '--no-daemon' '--no-color' '--folder' '--archived' '--unread' '-n') ;;
      completion) candidates=('bash' 'zsh' 'fish' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      contact) candidates=('add' 'block' 'list' 'remove' 'search' 'unblock') ;;
      daemon) candidates=('restart' 'run' 'status' 'stop') ;;
      doctor) candidates=('--json' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      download) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color' '-O') ;;
      fetch) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color' '--limit' '--all' '--since') ;;
      folder) candidates=('add-chat' 'create' 'delete' 'edit' 'list' 'remove-chat' 'show') ;;
      history) candidates=('--account' '--json' '--timeout' '--cursor' '--verbose' '-v' '--no-daemon' '--no-color' '-n' '--before' '--since' '--until' '--topic' '--local') ;;
      listen) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color' '--chat' '--types' '--count') ;;
      login) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color' '--qr' '--bot') ;;
      logout) candidates=('--account' '--json' '--allow-write' '--dry-run' '--yes' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      me) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      msg) candidates=('delete' 'edit' 'forward' 'get' 'link' 'pin' 'react' 'unpin') ;;
      read) candidates=('--account' '--json' '--timeout' '--cursor' '--verbose' '-v' '--no-daemon' '--no-color' '-n' '--before' '--since' '--until' '--topic' '--local') ;;
      resolve) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      saved) candidates=('attach' 'search' 'tags') ;;
      schema) candidates=('--json' '--verbose' '-v' '--no-daemon' '--no-color' '--all') ;;
      search) candidates=('--account' '--json' '--timeout' '--cursor' '--verbose' '-v' '--no-daemon' '--no-color' '--chat' '--global' '--from' '--type' '-n') ;;
      send) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--md' '--html' '--reply-to' '--topic' '--silent' '--schedule') ;;
      session) candidates=('list' 'terminate') ;;
      storage) candidates=('optimize' 'stats') ;;
      topic) candidates=('close' 'create' 'edit' 'list' 'reopen') ;;
      unread) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      version) candidates=('--json' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      wait-for) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color' '--chat' '--from' '--regex' '--after') ;;
  esac
else
  key="$words[2] $words[3]"
  case "$key" in
      account\ add) candidates=('--json' '--verbose' '-v' '--no-color') ;;
      account\ list) candidates=('--json' '--verbose' '-v' '--no-color') ;;
      account\ remove) candidates=('--json' '--verbose' '-v' '--no-color' '--keep-session' '--reassign-default' '--yes') ;;
      account\ show) candidates=('--json' '--verbose' '-v' '--no-color') ;;
      account\ use) candidates=('--json' '--verbose' '-v' '--no-color') ;;
      chat\ archive) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ ban) candidates=('--account' '--json' '--allow-write' '--dry-run' '--yes' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ demote) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ info) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ invite-link) candidates=('--account' '--json' '--allow-write' '--dry-run' '--yes' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color' '--revoke') ;;
      chat\ join) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ kick) candidates=('--account' '--json' '--allow-write' '--dry-run' '--yes' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ leave) candidates=('--account' '--json' '--allow-write' '--dry-run' '--yes' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ mark-read) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ members) candidates=('--account' '--json' '--timeout' '--cursor' '--verbose' '-v' '--no-daemon' '--no-color' '--admins' '--bots' '--query' '-n') ;;
      chat\ mute) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--for') ;;
      chat\ pin) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ promote) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--rights') ;;
      chat\ set-description) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ set-permissions) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--permissions') ;;
      chat\ set-photo) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--delete') ;;
      chat\ set-title) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ unarchive) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ unban) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ unmute) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chat\ unpin) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      chats) candidates=('--account' '--json' '--timeout' '--cursor' '--verbose' '-v' '--no-daemon' '--no-color' '--folder' '--archived' '--unread' '-n') ;;
      completion) candidates=('bash' 'zsh' 'fish' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      contact\ add) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      contact\ block) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      contact\ list) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      contact\ remove) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      contact\ search) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      contact\ unblock) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      daemon\ restart) candidates=('--json' '--verbose' '-v' '--no-color') ;;
      daemon\ run) candidates=('--json' '--verbose' '-v' '--no-color') ;;
      daemon\ status) candidates=('--json' '--verbose' '-v' '--no-color') ;;
      daemon\ stop) candidates=('--json' '--verbose' '-v' '--no-color') ;;
      doctor) candidates=('--json' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      download) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color' '-O') ;;
      fetch) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color' '--limit' '--all' '--since') ;;
      folder\ add-chat) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      folder\ create) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--chat' '--icon' '--color') ;;
      folder\ delete) candidates=('--account' '--json' '--allow-write' '--dry-run' '--yes' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      folder\ edit) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--name' '--icon' '--color') ;;
      folder\ list) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      folder\ remove-chat) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      folder\ show) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      history) candidates=('--account' '--json' '--timeout' '--cursor' '--verbose' '-v' '--no-daemon' '--no-color' '-n' '--before' '--since' '--until' '--topic' '--local') ;;
      listen) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color' '--chat' '--types' '--count') ;;
      login) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color' '--qr' '--bot') ;;
      logout) candidates=('--account' '--json' '--allow-write' '--dry-run' '--yes' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      me) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      msg\ delete) candidates=('--account' '--json' '--allow-write' '--dry-run' '--yes' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--for-all') ;;
      msg\ edit) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      msg\ forward) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--drop-author') ;;
      msg\ get) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      msg\ link) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      msg\ pin) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      msg\ react) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--remove' '--big') ;;
      msg\ unpin) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      read) candidates=('--account' '--json' '--timeout' '--cursor' '--verbose' '-v' '--no-daemon' '--no-color' '-n' '--before' '--since' '--until' '--topic' '--local') ;;
      resolve) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      saved\ attach) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--caption') ;;
      saved\ search) candidates=('--account' '--json' '--timeout' '--cursor' '--verbose' '-v' '--no-daemon' '--no-color' '--tag' '-n') ;;
      saved\ tags) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      schema) candidates=('--json' '--verbose' '-v' '--no-daemon' '--no-color' '--all') ;;
      search) candidates=('--account' '--json' '--timeout' '--cursor' '--verbose' '-v' '--no-daemon' '--no-color' '--chat' '--global' '--from' '--type' '-n') ;;
      send) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--md' '--html' '--reply-to' '--topic' '--silent' '--schedule') ;;
      session\ list) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      session\ terminate) candidates=('--account' '--json' '--allow-write' '--dry-run' '--yes' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      storage\ optimize) candidates=('--account' '--json' '--allow-write' '--dry-run' '--yes' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      storage\ stats) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      topic\ close) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      topic\ create) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color' '--icon') ;;
      topic\ edit) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      topic\ list) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      topic\ reopen) candidates=('--account' '--json' '--allow-write' '--dry-run' '--timeout' '--idempotency-key' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      unread) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      version) candidates=('--json' '--verbose' '-v' '--no-daemon' '--no-color') ;;
      wait-for) candidates=('--account' '--json' '--timeout' '--verbose' '-v' '--no-daemon' '--no-color' '--chat' '--from' '--regex' '--after') ;;
  esac
fi
_describe 'tgcli value' candidates
)TGZSH";
inline constexpr std::string_view kFishCompletion = R"TGFISH(# Generated from docs/commands/public-command-registry.json. Do not edit.
function __tgcli_state
  set -l top ''
  set -l child ''
  set -l pending 0
  set -l index 2
  while test $index -le (count $argv)
    set -l token $argv[$index]
    if test $pending -eq 1
      set pending 0
    else
      switch $token
        case '--account' '--timeout' '--cursor' '--idempotency-key'
          set pending 1
        case '--account=*' '--timeout=*' '--cursor=*' '--idempotency-key=*' '--json' '--allow-write' '--yes' '--dry-run' '--verbose' '-v' '--no-daemon' '--no-color' '-*'
        case '*'
          if test -z "$top"
            switch $token
              case 'account' 'chat' 'chats' 'completion' 'contact' 'daemon' 'doctor' 'download' 'fetch' 'folder' 'history' 'listen' 'login' 'logout' 'me' 'msg' 'read' 'resolve' 'saved' 'schema' 'search' 'send' 'session' 'storage' 'topic' 'unread' 'version' 'wait-for'
                set top $token
            end
          else if test -z "$child"
            switch "$top:$token"
              case 'account:add' 'account:list' 'account:remove' 'account:show' 'account:use' 'chat:archive' 'chat:ban' 'chat:demote' 'chat:info' 'chat:invite-link' 'chat:join' 'chat:kick' 'chat:leave' 'chat:mark-read' 'chat:members' 'chat:mute' 'chat:pin' 'chat:promote' 'chat:set-description' 'chat:set-permissions' 'chat:set-photo' 'chat:set-title' 'chat:unarchive' 'chat:unban' 'chat:unmute' 'chat:unpin' 'contact:add' 'contact:block' 'contact:list' 'contact:remove' 'contact:search' 'contact:unblock' 'daemon:restart' 'daemon:run' 'daemon:status' 'daemon:stop' 'folder:add-chat' 'folder:create' 'folder:delete' 'folder:edit' 'folder:list' 'folder:remove-chat' 'folder:show' 'msg:delete' 'msg:edit' 'msg:forward' 'msg:get' 'msg:link' 'msg:pin' 'msg:react' 'msg:unpin' 'saved:attach' 'saved:search' 'saved:tags' 'session:list' 'session:terminate' 'storage:optimize' 'storage:stats' 'topic:close' 'topic:create' 'topic:edit' 'topic:list' 'topic:reopen'
                set child $token
            end
          end
      end
    end
    set index (math $index + 1)
  end
  echo "$top|$child|$pending"
end
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''||0'\''' -a 'account chat chats completion contact daemon doctor download fetch folder history listen login logout me msg read resolve saved schema search send session storage topic unread version wait-for'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''account||0'\''' -a 'add list remove show use'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat||0'\''' -a 'archive ban demote info invite-link join kick leave mark-read members mute pin promote set-description set-permissions set-photo set-title unarchive unban unmute unpin'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''contact||0'\''' -a 'add block list remove search unblock'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''daemon||0'\''' -a 'restart run status stop'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''folder||0'\''' -a 'add-chat create delete edit list remove-chat show'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''msg||0'\''' -a 'delete edit forward get link pin react unpin'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''saved||0'\''' -a 'attach search tags'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''session||0'\''' -a 'list terminate'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''storage||0'\''' -a 'optimize stats'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''topic||0'\''' -a 'close create edit list reopen'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''account|add|0'\''' -a '--json --verbose -v --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''account|list|0'\''' -a '--json --verbose -v --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''account|remove|0'\''' -a '--json --verbose -v --no-color --keep-session --reassign-default --yes'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''account|show|0'\''' -a '--json --verbose -v --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''account|use|0'\''' -a '--json --verbose -v --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|archive|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|ban|0'\''' -a '--account --json --allow-write --dry-run --yes --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|demote|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|info|0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|invite-link|0'\''' -a '--account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color --revoke'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|join|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|kick|0'\''' -a '--account --json --allow-write --dry-run --yes --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|leave|0'\''' -a '--account --json --allow-write --dry-run --yes --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|mark-read|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|members|0'\''' -a '--account --json --timeout --cursor --verbose -v --no-daemon --no-color --admins --bots --query -n'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|mute|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --for'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|pin|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|promote|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --rights'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|set-description|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|set-permissions|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --permissions'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|set-photo|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --delete'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|set-title|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|unarchive|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|unban|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|unmute|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chat|unpin|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''chats||0'\''' -a '--account --json --timeout --cursor --verbose -v --no-daemon --no-color --folder --archived --unread -n'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''completion||0'\''' -a 'bash zsh fish --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''contact|add|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''contact|block|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''contact|list|0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''contact|remove|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''contact|search|0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''contact|unblock|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''daemon|restart|0'\''' -a '--json --verbose -v --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''daemon|run|0'\''' -a '--json --verbose -v --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''daemon|status|0'\''' -a '--json --verbose -v --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''daemon|stop|0'\''' -a '--json --verbose -v --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''doctor||0'\''' -a '--json --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''download||0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color -O'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''fetch||0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color --limit --all --since'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''folder|add-chat|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''folder|create|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --chat --icon --color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''folder|delete|0'\''' -a '--account --json --allow-write --dry-run --yes --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''folder|edit|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --name --icon --color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''folder|list|0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''folder|remove-chat|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''folder|show|0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''history||0'\''' -a '--account --json --timeout --cursor --verbose -v --no-daemon --no-color -n --before --since --until --topic --local'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''listen||0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color --chat --types --count'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''login||0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color --qr --bot'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''logout||0'\''' -a '--account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''me||0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''msg|delete|0'\''' -a '--account --json --allow-write --dry-run --yes --timeout --idempotency-key --verbose -v --no-daemon --no-color --for-all'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''msg|edit|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''msg|forward|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --drop-author'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''msg|get|0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''msg|link|0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''msg|pin|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''msg|react|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --remove --big'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''msg|unpin|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''read||0'\''' -a '--account --json --timeout --cursor --verbose -v --no-daemon --no-color -n --before --since --until --topic --local'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''resolve||0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''saved|attach|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --caption'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''saved|search|0'\''' -a '--account --json --timeout --cursor --verbose -v --no-daemon --no-color --tag -n'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''saved|tags|0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''schema||0'\''' -a '--json --verbose -v --no-daemon --no-color --all'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''search||0'\''' -a '--account --json --timeout --cursor --verbose -v --no-daemon --no-color --chat --global --from --type -n'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''send||0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --md --html --reply-to --topic --silent --schedule'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''session|list|0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''session|terminate|0'\''' -a '--account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''storage|optimize|0'\''' -a '--account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''storage|stats|0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''topic|close|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''topic|create|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color --icon'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''topic|edit|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''topic|list|0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''topic|reopen|0'\''' -a '--account --json --allow-write --dry-run --timeout --idempotency-key --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''unread||0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''version||0'\''' -a '--json --verbose -v --no-daemon --no-color'
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''wait-for||0'\''' -a '--account --json --timeout --verbose -v --no-daemon --no-color --chat --from --regex --after'
)TGFISH";
