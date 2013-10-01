Diagnostic Log and Trace
========================

Diagnostic Log and Trace (DLT) is a logging mechanism defined and
implemented by GENIVI: http://projects.genivi.org/diagnostic-log-trace/

SyncEvolution optionally supports DLT as follows:

 * syncevo-dbus-server, syncevo-dbus-helper and syncevo-local-sync can
   log to DLT. Operations with "syncevolution --daemon=no" never use
   DLT.
 * Each of the three processes uses a different application ID. By
   default, these IDs are "SYNS", "SYNH", "SYNL". These default can be
   changed via configure options. All processes use just one context,
   with the fixed ID "SYNC".
 * syncevo-dbus-helper and syncevo-local-sync only run occasionally.
   This makes is hard to adjust their log levels, for example via the
   dlt-viewer, because the processes and their contexts are only shown
   (known?) while the processes run. To work around this, the initial
   log level of these two helpers are inherited from the
   log level of the "SYNC" context in syncevo-dbus-helper.
 * That log level defaults to "WARN", which ensures that normal runs
   produce no output.
 * To enable DLT support during compilation, use
   "--enable-dlt" and "--with-dlt-syncevolution=SYNS,SYNH,SYNL" where SYNS/H/L
   are the actual application IDs.
 * To enable DLT support at runtime, run syncevo-dbus-server with
   "--dlt". Logging to syslog should be disabled with "--no-syslog".
 * The hierarchical log from libsynthesis gets flattened into a flat
   stream of messages and no syncevolution-log.html files are written.
