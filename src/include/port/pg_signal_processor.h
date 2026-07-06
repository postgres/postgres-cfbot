#ifndef PG_SIGNAL_PROCESSOR
#define PG_SIGNAL_PROCESSOR

typedef void (*pg_signal_processor_handler) (int signo);

extern void pg_signal_processor_set_serial_handler(int signo,
												   pg_signal_processor_handler handler);
extern int	pg_signal_processor_start(void);
extern void pg_signal_processor_stop(void);

#endif
