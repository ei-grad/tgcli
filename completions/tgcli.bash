# Generated from docs/commands/public-command-registry.json. Do not edit.
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
            case "$token" in account|chat|chats|completion|contact|daemon|doctor|download|fetch|folder|history|listen|login|logout|me|msg|raw|read|resolve|saved|schema|search|send|session|storage|topic|unread|version|wait-for) top="$token" ;; esac
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
    candidates='account chat chats completion contact daemon doctor download fetch folder history listen login logout me msg raw read resolve saved schema search send session storage topic unread version wait-for'
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
      raw) candidates='- --account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color' ;;
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
      raw) candidates='- --account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color' ;;
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
