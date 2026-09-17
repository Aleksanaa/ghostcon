# kmscon shell integration for bash
#
# Reports where prompts, commands and command output start and end using the
# OSC 133 sequences. This lets kmscon jump between prompts and select the
# output of a single command.
#
# Source this from ~/.bashrc:
#	source /usr/share/kmscon/shell-integration/kmscon-integration.bash

# Only useful in an interactive shell on a terminal that understands it
case "$-" in
*i*) ;;
*) return 0 ;;
esac
[ -n "$KMSCON_SHELL_INTEGRATION" ] || return 0

# Guard against being sourced twice, which would duplicate the markers
[ -n "$__kmscon_integration_loaded" ] && return 0
__kmscon_integration_loaded=1
__kmscon_at_prompt=
__kmscon_command_ran=

__kmscon_prompt_marker='\[\033]133;B\007\]'

# End of the output of the previous command, then the start of a new prompt
__kmscon_precmd() {
	local ret=$?

	if [ -n "$__kmscon_command_ran" ]; then
		printf '\033]133;D;%s\007' "$ret"
		__kmscon_command_ran=
	fi
	printf '\033]133;A\007'

	# The prompt ends where the user starts typing. This is done here and
	# not once at startup because a config that sets PS1 after sourcing
	# this file would drop the marker again.
	case "$PS1" in
	*"$__kmscon_prompt_marker") ;;
	*) PS1="$PS1$__kmscon_prompt_marker" ;;
	esac

	__kmscon_at_prompt=1
}

# The output of the command the user just entered starts here. The DEBUG trap
# also runs for the prompt hook and for completion, and more than once per
# command line, so only the first one after a prompt counts.
__kmscon_preexec() {
	[ -n "$COMP_LINE" ] && return
	[ -z "$__kmscon_at_prompt" ] && return

	__kmscon_at_prompt=
	__kmscon_command_ran=1
	printf '\033]133;C\007'
}

PROMPT_COMMAND="__kmscon_precmd${PROMPT_COMMAND:+;$PROMPT_COMMAND}"
trap '__kmscon_preexec' DEBUG
