/*
 * psql - the PostgreSQL interactive terminal
 *
 * Copyright (c) 2000-2026, PostgreSQL Global Development Group
 *
 * src/bin/psql/prompt.h
 */
#ifndef PROMPT_H
#define PROMPT_H

#include "fe_utils/conditional.h"
/* enum promptStatus_t is now defined by psqlscan.h */
#include "fe_utils/psqlscan.h"

char	   *get_prompt(promptStatus_t status, ConditionalStack cstack);
void		run_prompt_command(void);
void		reset_prompt_status_after_connect(void);

#endif							/* PROMPT_H */
