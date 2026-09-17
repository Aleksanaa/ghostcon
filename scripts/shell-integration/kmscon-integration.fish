# kmscon shell integration for fish
#
# Reports where prompts, commands and command output start and end using the
# OSC 133 sequences. This lets kmscon jump between prompts and select the
# output of a single command.
#
# Source this at the end of ~/.config/fish/config.fish, after your own prompt
# has been defined:
#	source /usr/share/kmscon/shell-integration/kmscon-integration.fish

status is-interactive; or return 0
set -q KMSCON_SHELL_INTEGRATION; or return 0

# fish reports prompts and commands by itself since 3.6, adding our own
# markers on top of that would report every prompt twice
set -l __kmscon_version (string split . -- $version)
if test "$__kmscon_version[1]" -gt 3
	return 0
else if test "$__kmscon_version[1]" -eq 3; and test "$__kmscon_version[2]" -ge 6
	return 0
end

# Guard against being sourced twice, which would duplicate the markers
set -q __kmscon_integration_loaded; and return 0
set -g __kmscon_integration_loaded 1

function __kmscon_prompt_start --on-event fish_prompt
	printf '\033]133;A\007'
end

function __kmscon_command_start --on-event fish_preexec
	printf '\033]133;C\007'
end

function __kmscon_command_end --on-event fish_postexec
	printf '\033]133;D;%s\007' $status
end

# The prompt ends where the user starts typing, so the marker goes after
# whatever fish_prompt prints
functions -c fish_prompt __kmscon_fish_prompt
function fish_prompt
	__kmscon_fish_prompt
	printf '\033]133;B\007'
end
