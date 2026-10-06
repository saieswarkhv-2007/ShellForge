#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <readline/history.h>
#include <readline/readline.h>

#include "history.h"
#include "token.h"
#include "lexer.h"
#include "parser.h"
#include "expand.h"
#include "builtin.h"
#include "executor.h"
#include "jobs.h"
#include "background.h"
#include "job_control.h"


int main(void)
{
    printf(
        "=====================================\n"
    );

    printf(
        "      Shellforge\n"
    );

    printf(
        " A Unix Style Shell written in C\n"
    );

    printf(
        "=====================================\n"
    );


    /*
     * Initialize job table.
     */

    jobs_init();


    /*
     * Initialize terminal job control.
     */

    job_control_init();


    /*
     * Install SIGCHLD handler.
     */

    setup_background_handler();


    using_history();


    token_list_t tokens;

    pipeline_t pipeline;

    char *line;


    while (1)
    {
        line =
            readline(
                "shellforge$ "
            );


        if (line == NULL)
        {
            printf(
                "\nGoodbye!\n"
            );

            break;
        }


        if (strlen(line) == 0)
        {
            free(line);

            continue;
        }


        /*
         * History builtin.
         */

        if (strcmp(
                line,
                "history"
            ) == 0)
        {
            print_history();

            free(line);

            continue;
        }


        add_history(line);


        /*
         * Tokenization.
         */

        lexer(
            line,
            &tokens
        );


        /*
         * Parsing.
         */

        if (parser(
                &tokens,
                &pipeline
            ))
        {
            /*
             * Variable expansion.
             */

            expand_variables(
                &pipeline
            );


            /*
             * Execute.
             */

            execute_pipeline(
                &pipeline
            );
        }


        free(line);
    }


    return 0;
}
