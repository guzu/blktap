/*
 * LTTng-UST tracepoint provider for libqcow2 ("qcow2" provider).
 *
 * This file must be compiled separately — it instantiates the tracepoint
 * probes defined in qcow2-tracepoints.h. Built only with --enable-lttng.
 */

#define TRACEPOINT_CREATE_PROBES
#define TRACEPOINT_DEFINE

#include "qcow2-tracepoints.h"
