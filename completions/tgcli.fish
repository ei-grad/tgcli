# Generated from docs/commands/public-command-registry.json. Do not edit.
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
              case 'account' 'chat' 'chats' 'completion' 'contact' 'daemon' 'doctor' 'download' 'fetch' 'folder' 'history' 'listen' 'login' 'logout' 'me' 'msg' 'raw' 'read' 'resolve' 'saved' 'schema' 'search' 'send' 'session' 'storage' 'topic' 'unread' 'version' 'wait-for'
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
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''||0'\''' -a 'account chat chats completion contact daemon doctor download fetch folder history listen login logout me msg raw read resolve saved schema search send session storage topic unread version wait-for'
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
complete -c tgcli -f -n 'test (__tgcli_state (commandline -opc)) = '\''raw||0'\''' -a '- --account --json --allow-write --dry-run --yes --timeout --verbose -v --no-daemon --no-color'
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
