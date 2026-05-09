/* todo.c - the user-side REPL for the todo app.

   the shape of this file is on purpose:
     - one tiny helper per concern (starts_with, equals, parse_int)
     - all user-facing strings as #defines up top so wording lives in
       one place
     - todo_run() is one big switch-by-prefix on the input line

   the actual work (storing tasks, printing the list, removing by id)
   happens kernel-side in src/isr80h/src_todo.c. everything in here is
   just "read a line, figure out which command it is, call the right
   peachos_todo_* wrapper, print success or failure".

   the wrappers themselves live in programs/stdlib/src/peachos.asm and
   are the int 0x80 trampolines into the kernel. */

#include "todo.h"
#include "peachos.h"
#include "string.h"

/* user-facing strings up here so it's easy to tweak wording without
   hunting through the dispatch logic below. */
#define MSG_WELCOME       "Todo App\n"
#define MSG_HINT          "Type 'help' for commands.\n\n"
#define MSG_PROMPT        "todo> "
#define MSG_BYE           "Bye\n"
#define MSG_UNKNOWN       "Unknown command\n"
#define MSG_USAGE_ADD     "Usage: add <task>\n"
#define MSG_BAD_ID        "ERROR: Invalid task ID\n"
#define MSG_ADD_OK        "Task added\n"
#define MSG_ADD_FAIL      "ERROR: Failed to add task\n"
#define MSG_LIST_FAIL     "ERROR: Failed to list tasks\n"
#define MSG_REMOVE_OK     "Task removed\n"
#define MSG_REMOVE_FAIL   "ERROR: Failed to remove task\n"

/* the help text. kept as a function instead of one giant printf so
   each line stays grep-able. */
static void print_help(void)
{
    print("Commands:\n");
    print("  add <task>\n");
    print("  list\n");
    print("  remove <id>\n");
    print("  help\n");
    print("  exit\n");
}

/* tiny string helpers - we deliberately don't pull in a full tokenizer.
   the command grammar is one verb per line, no quoting, no escapes. */
static int starts_with(const char* text, const char* prefix)
{
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

/* equals(): match a whole word, but accept either NUL or '\n' as the
   terminator. peachos_terminal_readline can leave a trailing newline
   in some configurations, and rather than mutate the buffer we just
   handle both endings here. */
static int equals(const char* text, const char* word)
{
    int n = strlen(word);
    return strncmp(text, word, n) == 0 && (text[n] == 0 || text[n] == '\n');
}

/* parse_int(): tiny non-negative integer parser. returns -1 on any
   non-digit so the caller can show a clean "invalid id" message
   instead of letting a bad value through to the syscall. validating
   here means the kernel sees fewer garbage calls. */
static int parse_int(const char* text, int* out)
{
    int len = strnlen(text, 32);
    if (len <= 0)
        return -1;

    int value = 0;
    for (int i = 0; i < len; i++)
    {
        if (!isdigit(text[i]))
            return -1;
        value = (value * 10) + tonumericdigit(text[i]);
    }

    *out = value;
    return 0;
}

/* todo_run - the REPL.
   each command follows the same little 3-step shape:
     1) check the input is well-formed
     2) call the kernel through a peachos_todo_* wrapper
     3) print success or failure
   keeping that shape consistent makes adding a new command basically
   copy-paste. */
int todo_run(void)
{
    print(MSG_WELCOME);
    print(MSG_HINT);

    while (1)
    {
        /* read one line. peachos_terminal_readline blocks until the
           user hits enter, and it echoes the typed characters for us. */
        print(MSG_PROMPT);
        char line[256];
        peachos_terminal_readline(line, sizeof(line), true);
        print("\n");

        /* empty line: just re-prompt instead of saying "unknown". */
        if (strnlen(line, sizeof(line)) == 0)
        {
            continue;
        }

        /* add <task> - take everything after "add " as the description. */
        if (starts_with(line, "add "))
        {
            const char* task_text = line + 4;
            if (strnlen(task_text, 64) == 0)
                print(MSG_USAGE_ADD);
            else if (peachos_todo_add(task_text) < 0)
                print(MSG_ADD_FAIL);
            else
                print(MSG_ADD_OK);
            continue;
        }

        /* list - kernel does the printing itself, since the data already
           lives in kernel memory. saves us a copy back across the
           syscall boundary. */
        if (equals(line, "list"))
        {
            if (peachos_todo_list() < 0)
                print(MSG_LIST_FAIL);
            continue;
        }

        /* remove <id> - validate the id locally first, then ask the
           kernel. "remove " has 7 chars including the space. */
        if (starts_with(line, "remove "))
        {
            int id = 0;
            if (parse_int(line + 7, &id) < 0)
                print(MSG_BAD_ID);
            else if (peachos_todo_remove(id) < 0)
                print(MSG_REMOVE_FAIL);
            else
                print(MSG_REMOVE_OK);
            continue;
        }

        if (equals(line, "help"))
        {
            print_help();
            continue;
        }

        /* exit - peachos_exit doesn't return, so the return 0 below is
           technically unreachable. it's there so todo_run() has a sane
           signature for main(). */
        if (equals(line, "exit"))
        {
            print(MSG_BYE);
            peachos_exit();
            return 0;
        }

        print(MSG_UNKNOWN);
    }

    return 0;
}
