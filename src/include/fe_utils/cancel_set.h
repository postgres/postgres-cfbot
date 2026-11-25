#ifndef CANCEL_SET
#define CANCEL_SET

#include "libpq-fe.h"

typedef struct cancel_set cancel_set;

extern cancel_set *cancel_set_alloc(void);
extern void cancel_set_free(cancel_set *e);

extern bool cancel_set_add(cancel_set *e, PGconn *connection);
extern bool cancel_set_remove(cancel_set *e, PGconn *connection);
extern bool cancel_set_cancel(cancel_set *e, PGconn *connection);
extern bool cancel_set_cancel_and_remove(cancel_set *e, PGconn *connection);

extern int	cancel_set_remove_all(cancel_set *e);
extern int	cancel_set_cancel_all(cancel_set *e);
extern int	cancel_set_cancel_and_remove_all(cancel_set *e);

#endif
