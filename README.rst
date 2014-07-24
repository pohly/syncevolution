===============
 SyncEvolution
===============

------------------------------------------------
synchronize personal information management data
------------------------------------------------

:Manual section: 1
:Version: 1.0
:Date: Apr 28, 2010

SYNOPSIS
========

List and manipulate databases:
  syncevolution --print-databases|--create-database|--remove-database [<properties>] [<config> <store>]

Show information about configuration(s):
  syncevolution --print-servers|--print-configs|--print-peers

Show information about a specific configuration:
  syncevolution --print-config [--quiet] [--] <config> [main|<store> ...]

List sessions:
  syncevolution --print-sessions [--quiet] [--] <config>

Show information about SyncEvolution:
  syncevolution --help|-h|--version

Run a synchronization as configured:
  syncevolution <config> [<store> ...]

Run a synchronization with properties changed just for this run:
  syncevolution --run <options for run> [--] <config> [<store> ...]

Restore data from the automatic backups:
  syncevolution --restore <session directory> --before|--after [--dry-run] [--] <config> <store> ...

Create, update or remove a configuration:
  syncevolution --configure <options> [--] <config> [<store> ...]

  syncevolution --remove|--migrate <options> [--] <config>

List items:
  syncevolution --print-items [--] [<config> [<store>]]

Export item(s):
  syncevolution [--delimiter <string>] --export <dir>|<file>|- [--] [<config> [<store> [<luid> ...]]]
                                                                --luids <luid> ...

Add item(s):
  syncevolution [--delimiter <string>|none] --import <dir>|<file>|- [--] [<config> [<store>]]
                                                                     --luids <luid> ...

Update item(s):
  syncevolution --update <dir> [--] <config> <store>

  syncevolution [--delimiter <string>|none] --update <file>|- [--] <config> <store> <luid> ...
                                                               --luids <luid> ...


Remove item(s):
  syncevolution --delete-items [--] <config> <store> (<luid> ... | '*')


DESCRIPTION
===========

This text explains the usage of the SyncEvolution command line.

SyncEvolution synchronizes personal information management (PIM) data
such as contacts, appointments, tasks and memos using the Synthesis
sync engine, which provides support for the SyncML synchronization
protocol.

SyncEvolution synchronizes with SyncML servers over HTTP and with
SyncML capable phones locally over Bluetooth (new in 1.0). Plugins
provide access to the data which is to be synchronized. Binaries are
available for Linux desktops (synchronizing data in GNOME Evolution,
with KDE supported indirectly already and Akonadi support in
development), for MeeGo (formerly Moblin) and for Maemo 5/Nokia
N900. The source code can be compiled for Unix-like systems and
provides a framework to build custom SyncML clients or servers.

TERMINOLOGY
===========

peer
  A peer is the entity that data is synchronized with. This can be
  another device (like a phone), a server (like Google) or
  even the host itself (useful for synchronizing two different
  databases).

host
  The device or computer that SyncEvolution runs on.

item
  The smallest unit of synchronization. Examples of items include
  calendar events and individual contacts, memos, or tasks.

database
  Each peer has one or more databases that get synchronized (Google Calendar,
  Google Contacts). Conceptually a database is a set of items where each
  item is independent of the others.

backend
  Access to databases is provided by SyncEvolution backends. It does
  not matter where that data is stored. Some backends provide access
  to data outside of the host itself (`CalDAV and CardDAV`_, ActiveSync).

datastore (or just "store")
  Used for the combination of SyncEvolution backend and database settings.
  A datastore provides read/write access to a database, which is a prerequisite
  for syncing the database. The datastore is independent of the peers that
  the database might be synchronized with.

  This used to be called "data source" or just "source", which is a
  term still found in older documentation, some file paths and the
  source code of SyncEvolution.

local/remote
  Synchronization always happens between a pair of databases and thus
  has two sides. One database or side of a sync is remote (the one
  of the peer), the other is local (SyncEvolution). For the sake of consistency (and
  lack of better terms), these terms are used even if the peer is another
  instance of SyncEvolution and/or all data resides on the same storage.

sync config
  A sync configuration defines how to talk with a peer: the protocol
  which is to be used, how to find the peer, credentials, etc.

  Sync configs can be used to initiate a sync (like contacting a
  SyncML server) or to handle an incoming sync request (when acting
  as SyncML server which is contacted by the peer).

  If the peer supports SyncML as sync protocol, a sync only uses one
  sync config on the SyncEvolution side. If the peer supports data
  access via some other protocols, then SyncEvolution can make that
  data available via SyncML and run a sync where SyncML is used
  internally.  Such a sync involves two sync configs, see ``originating
  config`` and ``target config``.

  A sync config can use all datastores defined in the same context
  (see below). Some properties of the datastore can be set differently
  for each peer and thus sync config (``per-peer``). One of these, the
  ``sync`` property, defines if and how a datastore is used during a
  sync.

context
  Sync and datastore configs are defined inside one or more configuration
  contexts. There is always a ``@default`` context that gets used if nothing
  else is specified.

  Typically each context represents a certain set of related
  datastores. For example, normally the ``@default`` context is used for
  local databases. Datastores related to a certain peer can
  be defined in a context ``@peer-name`` named after that peer.

configuration properties
  SyncEvolution uses key/value pairs to store configuration options.
  A configuration is a set of unique keys and their values that together
  describe a certain object.

  These sets of properties are addressed via the main config name (a
  sync config name with or without an explicit context, or just the
  context name) and optionally the datastore name (if the properties
  are for a specific datastore).

  Sync properties are set for sync configs, independently of a
  particular datastore. Properties that cannot be set without
  specifying they datastore that they apply to are datastore
  properties. This includes properties that belong both to a datastore
  and a sync config.

  The property names were chosen so that they are unique, i.e., no
  sync property has the same name as a datastore property. For historic
  reasons, internally these properties are treated as two different
  sets and there are two different command line options to query the
  list of sync resp. datastore properties.

  Some configuration properties are shared between configurations
  automatically. This sharing is hard-coded and cannot be configured.
  It has the advantage that certain settings only need to be set
  once and/or can be changed for several different configs
  at once.

  A property can be *unshared* (has separate values for each peer, therefore
  sometimes also called *per-peer*; for example the ``sync`` property),
  *shared* (same value for all peers; for
  example the ``database`` property for selecting the local database) or
  *global* (exactly one value).

  Together with the distinction between sync and datastore properties,
  this currently results in five different groups of properties:

  * Sync properties (by definition, this also includes properties
    independent of a particular sync config because they are set for
    all sync configs at once, independently of any particular
    datastore):

    * global (= ``~/.config/syncevolution/config.ini``):
      independent of a particular context, for example ``keyring``
    * shared (= ``~/.config/syncevolution/<context name>/config.ini``):
      set once for each context, for example ``logdir``
    * unshared (= ``~/.config/syncevolution/<context name>/peers/<peer name>/config.ini``):
      set separately for each sync config, for example ``syncURL``

  * Datastore properties:

    * shared (= ``~/.config/syncevolution/<context name>/sources/<store name>/config.ini``):
      the properties required for access to the data, primarily ``backend`` and ``database``
    * unshared (= ``~/.config/syncevolution/<context name>/peers/<peer name>/sources/<store name>/config.ini``):
      the already mentioned ``sync`` and ``uri`` properties, but also a per-peer
      sync format properties

  Many properties have reasonable defaults, either defined in the
  configuration layer or chosen at runtime by the SyncEvolution
  engine reading the configuration, and therefore do not have to
  be set.

  The configuration layer in SyncEvolution has a very limited
  understanding of the semantic of each property. It just knows about
  some generic types (strings, boolean, integers, ...) and where
  properties are supposed to be stored. It is the layer above that,
  the one which actually tries to use the configuration, that
  determines whether the property values make sense as
  specified. Beware that it is possible to set properties to values
  that conflict with other property values (triggering errors when
  using the configuration) or to set properties that are not used
  (typically they get ignored silently, unless an explicit error check
  was implemented).

configuration template
  Templates define the settings for specific peers. Some templates
  are packaged together with SyncEvolution, others may be added by
  packagers or users. Settings from templates are copied once into
  the sync config when creating it. There is no permanent link back
  to the template, so updating a template has no effect on configs
  created from it earlier.

  A template only contains unshared properties. Therefore it is
  possible to first set shared properties (for example, choosing
  which databases to synchronize in the default context), then
  add sync configs for different peers to that context without
  reseting the existing settings.

  In SyncEvolution's predefined configuration templates, the following
  names for datastores are used. Different names can be chosen for datastores
  that are defined manually.

  * addressbook: a list of contacts
  * calendar: calendar *events*
  * memo: plain text notes
  * todo: task list
  * calendar+todo: a virtual datastore combining one local "calendar" and
    one "todo" datastore (required for synchronizing with some phones)

local sync
  Traditionally, a sync config specifies SyncML as the synchronization
  protocol via the `syncURL` property. The peer must support SyncML for
  this to work.

  In a so called local sync, SyncEvolution acts as SyncML server
  and client at the same time, connecting the two sides via internal
  message passing. Both sides have their own set of datastores, which may
  use CalDAV, CardDAV or ActiveSync to access the data.

  See `Synchronization beyond SyncML`_.

originating config
  In a local sync, the sync config used to start the sync is called
  the originating sync config, or just originating config.

target config
  In addition to the originating config, a local sync also uses a target
  config. At the configuration level, this target config is just another
  sync config. It becomes a target config when referenced by a sync config
  for local syncing.


COMMAND LINE CONVENTIONS
========================

The ``<config>`` and the ``<store>`` strings in the command line synopsis are
used to find the sync resp. datastore configs. Depending on which
other parameters are given, different operations are executed.

The ``<config>`` string has the format ``[<peer>][@<context>]``. When
the context is not specified explicitly, SyncEvolution first searches
for an existing sync configuration with the given ``<peer>`` name. If
not found, the configuration can only be created, but not read. It
will be created in the ``@default`` context as fallback. The empty
``<config>`` string is an alias for ``@default``.

The ``<peer>`` part identifies a specific sync or target config inside
the context. It is optional and does not have to be specified when not
needed, for example when configuring the shared settings of datastores
(``--configure @default addressbook``) or accessing items inside a
datastore (``--print-items @work calendar``).

Listing datastores on the command line limits the operation to those
datastores (called *active datastores* below). If not given, all datastores
enabled for the config are active. Some operations require
the name of exactly one datastore.

Properties are set with key/value assignments and/or the
``--sync/store-property`` keywords. Those keywords are only needed for
the hypothetical situation that a sync and datastore property share the
same name (which was intentionally avoided). Without them, SyncEvolution
automatically identifies which kind of property is meant based on the
name.

A ``<property>`` assignment has the following format::

  [<store>/]<name>[@<context>|@<peer>@<context>]=<value>

The optional ``<context>`` or ``<peer>@<context>`` suffix limits the scope
of the value to that particular configuration. This is useful when
running a local sync, which involves a sync and a target
configuration. For example, the log level can be specified separately
for both sides::

  --run loglevel@default=1 loglevel@google-calendar=4 google-calendar@default

A string without a second @ sign inside is always interpreted as a
context name, so in contrast to the ``<config>`` string, ``foo`` cannot be
used to reference the ``foo@default`` configuration. Use the full name
including the context for that.

When no config or context is specified explicitly, a value is
changed in all active configs, typically the one given with
``<config>``.  The priority of multiple values for the same config
is `more specific definition wins`, so ``<peer>@<context>``
overrides ``@<context>``, which overrides `no suffix given`.
Specifying some suffix which does not apply to the current operation
does not trigger an error, so beware of typos.

Datastore properties can be specified with a ``<store>/`` prefix. This
allows limiting the value to the selected datastore. For example::

  --configure "addressbook/database=My Addressbook" \
              "calendar/database=My Calendar" \
              @default addressbook calendar

Another way to achieve the same effect is to run the ``--configure``
operation twice, once for ``addressbook`` and once for ``calendar``::

  --configure "database=My Addressbook" @default addressbook
  --configure "database=My Calendar" @default calendar

If the same property is set both with and without a ``<store>/`` prefix,
then the more specific value with that prefix is used for that datastore,
regardless of the order on the command line. The following command
enables all datastores except for the addressbook::

    --configure addressbook/sync=none \
                sync=two-way \
                <sync config>


USAGE
=====

::

   syncevolution --print-databases [<properties>] [<config> <store>]

If no additional arguments are given, then SyncEvolution will list all
available backends and the databases that can be accessed through each
backend. This works without existing configurations. However, some
backends, like for example the CalDAV backend, need additional
information (like credentials or URL of a remote server). This
additional information can be provided on the command line with
property assignments (``username=...``) or in an existing configuration.

When listing all databases of all active datastores, the output starts
with a heading that lists the values for the ``backend`` property which
select the backend, followed by the databases.  Each database has a
name and a unique ID (in brackets). Typically both can be used as
value of the 'database' property. One database might be marked as
``default``. It will be used when ``database`` is not set explicitly.

When selecting an existing datastore configuration or specifying the ``backend``
property on the command line, only the databases for that backend
are listed and the initial line shows how that backend was selected
(<config>/<store> resp. backend value).

Some backends do not support listing of databases. For example, the
file backend synchronizes directories with one file per item and
always needs an explicit ``database`` property because it cannot guess
which directory it is meant to use. ::

   syncevolution --create-database [<properties>] [<config> <store>]

Creates a new database for the selected ``backend``, using the
information given in the ``database`` property. As with
``--print-databases``, it is possible to give the properties directly
without configuring a datastore first.

The interpretation of the ``database`` property depends on the
backend. Not all backends support this operation.

The EDS backend uses the value of the ``database`` as name of the new
database and assigns a unique URI automatically. ::

   syncevolution --remove-database [<properties>] [<config> <store>]

Looks up the database based on the ``database`` property (depending
on the backend, both name and a URI are valid), then deletes the data.
Note that datastore configurations using the database are not removed. ::

   syncevolution <config>

Without the optional list of datastores, all datastores which are enabled in
their configuration file are synchronized. ::

   syncevolution <config> <store> ...

Otherwise only the ones mentioned on the command line are active. It
is possible to configure datastores without activating their
synchronization: if the synchronization mode of a datastore is set to
`disabled`, the datastore will be ignored. Explicitly listing such a
datastore will synchronize it in `two-way` mode once.

Progress and error messages are written into a log file that is
preserved for each synchronization run. Details about that is found in
the `Automatic Backups and Logging` section below. All errors and
warnings are printed directly to the console in addition to writing
them into the log file. Before quitting SyncEvolution will print a
summary of how the local data was modified.  This is done with the
`synccompare` utility script described in the `Exchanging Data`_
section.

When the ``logdir`` property is enabled (since v0.9 done by default for
new configurations), then the same comparison is also done before the
synchronization starts.

In case of a severe error the synchronization run is aborted
prematurely and SyncEvolution will return a non-zero value. Recovery
from failed synchronization is done by forcing a full synchronization
during the next run, i.e. by sending all items and letting the SyncML
server compare against the ones it already knows. This is avoided
whenever possible because matching items during a slow synchronization
can lead to duplicate entries.

After a successful synchronization the server's configuration file is
updated so that the next run can be done incrementally.  If the
configuration file has to be recreated e.g. because it was lost, the
next run recovers from that by doing a full synchronization. The risk
associated with this is that the server might not recognize items that
it already has stored previously which then would lead to duplication
of items. ::

   syncevolution --configure <options for configuration> <config> [<store> ...]

Options in the configuration can be modified via the command
line. The <config> and the optional <store> parameters define
what gets created or modified. The remaining parameters define
which values get set or modified.

To change settings of specific datastores, either invoke syncevolution
multiple times with exactly one <store> parameter or use the
``[<store>/]`` prefix described above for property assignments. ::

   syncevolution --remove <config>

Deletes the configuration. If the <config> refers to a specific
peer, only that peer's configuration is removed. If it refers to
a context, that context and all peers and datastores defined inside
it are removed.

Note that there is no confirmation question. Neither local data
referenced by the configuration nor the content of log dirs are
deleted. ::

   syncevolution --run <options for run> <config> [<store> ...]

Options can also be overridden for just the current run, without
changing the configuration. In order to prevent accidentally running a
sync session when a configuration change was intended, either
--configure or --run must be given explicitly if options are specified
on the command line. ::

   syncevolution --status <config> [<store> ...]

Prints what changes were made locally since the last synchronization.
Depends on access to database dumps from the last run, so enabling the
``logdir`` property is recommended. ::

   syncevolution --print-servers|--print-configs|--print-peers
   syncevolution --print-config [--quiet] <config> [main|<store> ...]
   syncevolution --print-sessions [--quiet] <config>

These commands print information about existing configurations. When
printing a configuration a short version without comments can be
selected with --quiet. When datastores are listed, only their
configuration is shown. `Main` instead or in combination with datastores
lists only the main peer configuration. ::

   syncevolution --restore <session directory> --before|--after
                 [--dry-run] <config> <store> ...

This restores local data from the backups made before or after a
synchronization session. The --print-sessions command can be used to
find these backups. The datastore(s) have to be listed explicitly. There
is intentionally no default, because as with --remove there is no
confirmation question. With --dry-run, the restore is only simulated.

The session directory has to be specified explicitly with its path
name (absolute or relative to current directory). It does not have to
be one of the currently active log directories, as long as it contains
the right database dumps for the selected datastores.

A restore tries to minimize the number of item changes (see section
`Item Changes and Data Changes`_). This means that items that are
identical before and after the change will not be transmitted anew to
the peer during the next synchronization. If the peer somehow
needs to get a clean copy of all local items, then use ``--sync
refresh-from-local`` in the next run. ::

  syncevolution --print-items <config> <store>
  syncevolution [--delimiter <string>] --export <dir>|<file>|- [<config> [<store> [<luid> ...]]]
  syncevolution [--delimiter <string>|none] --import <dir>|<file>|- [<config> <store>]
  syncevolution --update <dir> <config> <store>
  syncevolution [--delimiter <string>|none] --update <file>|- <config> <store> <luid> ...
  syncevolution --delete-items <config> <store> (<luid> ... | *)

Restore depends on the specific format of the automatic backups
created by SyncEvolution. Arbitrary access to item data is provided
with additional options. <luid> here is the unique local identifier
assigned to each item in the datastore, transformed so that it contains
only alphanumeric characters, dash and underscore. A star * in
--delete-items selects all items for deletion. There are two ways
of specifying luids: either as additional parameters after the
config and datastore parameters (which may be empty in this case, but
must be given) or after the ``--luids`` keyword.

<config> and <store> may be given to define the database which is to
be used. If not given or not refering to an existing configuration
(which is not an error, due to historic reasons), the desired backend
must be given via the ``backend`` property, like this::

  syncevolution --print-items backend=evolution-contacts
  syncevolution --export - backend=evolution-contacts \
                --luids pas-id-4E33F24300000006 pas-id-4E36DD7B00000007

The desired backend database can be chosen via ``database=<identifier>``.
See ``--print-databases``.

OPTIONS
=======

Here is a full description of all <options> that can be put in front
of the server name. Whenever an option accepts multiple values, a
question mark can be used to get the corresponding help text and/or
a list of valid values.

--sync|-s <mode>|?
  Temporarily synchronize the active datastores in that mode. Useful
  for a `refresh-from-local` or `refresh-from-remote` sync which
  clears all data at one end and copies all items from the other.

  **Warning:** `local` is the data accessed via the sync config
  directly and `remote` is the data on the peer, regardless
  where the data is actually stored physically.

--print-servers|--print-configs|--print-peers
  Prints the names of all configured peers to stdout. There is no
  difference between these options, the are just aliases.

--print-servers|--print-configs|--print-peers|-p
  Prints the complete configuration for the selected <config>
  to stdout, including up-to-date comments for all properties. The
  format is the normal .ini format with datastore configurations in
  different sections introduced with [<store>] lines. Can be combined
  with --sync-property and --datastore-property to modify the configuration
  on-the-fly. When one or more datastores are listed after the <config>
  name on the command line, then only the configs of those datastores are
  printed. `main` selects the main configuration instead of datastore
  configurations. Using --quiet suppresses the comments for each property.
  When setting a --template, then the reference configuration for
  that peer is printed instead of an existing configuration.

\--print-sessions
  Prints information about previous synchronization sessions for the
  selected peer or context are printed. This depends on the ``logdir``
  property.  The information includes the log directory name (useful for
  --restore) and the synchronization report. In combination with
  --quiet, only the paths are listed.

--configure|-c
  Modify the configuration files for the selected peer and/or datastores.

  If no such configuration exists, then a new one is created using one
  of the template configurations (see --template option). Choosing a
  template sets most of the relevant properties for the peer and the
  default set of datastores (see above for a list of those). Anything
  specific to the user (like username/password) still has to be set
  manually.

  When creating a new configuration and listing datastores explicitly on the
  command line, only those datastores will be set to active in the new
  configuration, i.e. `syncevolution -c memotoo addressbook`
  followed by `syncevolution memotoo` will only synchronize the
  address book. The other datastores are created in a disabled state.
  When modifying an existing configuration and datastores are specified,
  then the datastore properties of only those datastores are modified.

  By default, creating a config requires a template. Datastore names on the
  command line must match those in the template. This allows catching
  typos in the peer and datastore names. But it also prevents some advanced
  use cases. Therefore it is possible to disable these checks in two ways::

    - use `--template none` or
    - specify all required sync and datastore properties that are normally
      in the templates on the command line (syncURL, backend, ...)

--run|-r
  To prevent accidental sync runs when a configuration change was
  intended, but the `--configure` option was not used, `--run` must be
  specified explicitly when sync or datastore properties are selected
  on the command line and they are meant to be used during a sync
  session triggered by the invocation.

\--migrate
  In older SyncEvolution releases a different layout of configuration files
  was used. Using --migrate will automatically migrate to the new
  layout and rename the <config> into <config>.old to prevent accidental use
  of the old configuration. WARNING: old SyncEvolution releases cannot
  use the new configuration!

  The switch can also be used to migrate a configuration in the current
  configuration directory: this preserves all property values, discards
  obsolete properties and sets all comments exactly as if the configuration
  had been created from scratch. WARNING: custom comments in the
  configuration are not preserved.

  --migrate implies --configure and can be combined with modifying
  properties.

\--print-items
  Shows all existing items using one line per item using
  the format "<luid>[: <short description>]". Whether the description
  is available depends on the backend and the kind of data that it
  stores.

\--export
  Writes all items in the datastore or all items whose <luid> is
  given into a directory if the --export parameter exists and is a
  directory. The <luid> of each item is used as file name. Otherwise it
  creates a new file under that name and writes the selected items
  separated by the chosen delimiter string. stdout can be selected with
  a dash.

  The default delimiter (two line breaks) matches a blank line. As a special
  case, it also matches a blank line with DOS line ending (line break,
  carriage return, line break). This works for vCard 3.0 and iCalendar 2.0,
  which never contain blank lines.

  When exporting, the default delimiter will always insert two line
  breaks regardless whether the items contain DOS line ends. As a
  special case, the initial newline of a delimiter is skipped if the
  item already ends in a newline.

\--import
  Adds all items found in the directory or input file to the
  datastore.  When reading from a directory, each file is treated as one
  item. Otherwise the input is split at the chosen delimiter. "none" as
  delimiter disables splitting of the input.

\--update
  Overwrites the content of existing items. When updating from a
  directory, the name of each file is taken as its luid. When updating
  from file or stdin, the number of luids given on the command line
  must match with the number of items in the input.

\--delete-items
  Removes the specified items from the datastore. Most backends print
  some progress information about this, but besides that, no further
  output is produced. Trying to remove an item which does not exist
  typically leads to an ERROR message, but is not reflected in a
  non-zero result of the command line invocation itself because the
  situation is not reported as an error by backends (removal of
  non-existent items is not an error in SyncML). Use a star \* instead
  or in addition to listing individual luids to delete all items.

--sync-property|-y <property>=<value>|<property>=?|?
  Overrides a datastore-independent configuration property for the
  current synchronization run or permanently when --configure is used
  to update the configuration. Can be used multiple times.  Specifying
  an unused property will trigger an error message.

--datastore-property|--source-property|-z <property>=<value>|<property>=?|?
  Same as --sync-property, but applies to the configuration of all active
  datastores. `--sync <mode>` is a shortcut for `--datastore-property sync=<mode>`.

--template|-l <peer name>|default|?<device>
  Can be used to select from one of the built-in default configurations
  for known SyncML peers. Defaults to the <config> name, so --template
  only has to be specified when creating multiple different configurations
  for the same peer, or when using a template that is named differently
  than the peer. `default` is an alias for `memotoo` and can be
  used as the starting point for servers which do not have a built-in
  template.

  A pseudo-random device ID is generated automatically. Therefore setting
  the `deviceId` sync property is only necessary when manually recreating a
  configuration or when a more descriptive name is desired.

  The available templates for different known SyncML servers are listed when
  using a single question mark instead of template name. When using the
  `?<device>` format, a fuzzy search for a template that might be
  suitable for talking to such a device is done. The matching works best
  when using `<device> = <Manufacturer> <Model>`. If you don't know the
  manufacturer, you can just keep it as empty. The output in this mode
  gives the template name followed by a short description and a rating how well
  the template matches the device (100% is best).

--status|-t
  The changes made to local data since the last synchronization are
  shown without starting a new one. This can be used to see in advance
  whether the local data needs to be synchronized with the server.

--quiet|-q
  Suppresses most of the normal output during a synchronization. The
  log file still contains all the information.

--keyring[=<value>]|-k
  A legacy option, now the same as setting the global keyring sync property.
  When not specifying a value explicitly, "true" for "use some kind of
  keyring" is implied. See "--sync-property keyring" for details.

--daemon[=yes/no]
  By default, the SyncEvolution command line is executed inside the
  syncevo-dbus-server process. This ensures that synchronization sessions
  started by the command line do not conflict with sessions started
  via some other means (GUI, automatically). For debugging purposes
  or very special use cases (running a local sync against a server which
  executes inside the daemon) it is possible to execute the operation
  without the daemon (--daemon=no).

--help|-h
  Prints usage information.

\--version
  Prints the SyncEvolution version.


CONFIGURATION PROPERTIES
========================

This section lists predefined properties. Backends can add their own
properties at runtime if none of the predefined properties are
suitable for a certain setting. Those additional properties are not
listed here. Use ``--sync/datastore-property ?`` to get an up-to-date
list.

The predefined properties may also be interpreted slightly differently
by each backend and sync protocol. Sometimes this is documented in the
comment for each property, sometimes in the documentation of the
backend or sync protocol.

Properties are listed together with all recognized aliases (in those
cases where a property was renamed at some point), its default value,
sharing state (unshared/shared/global). Some properties must be
defined, which is marked with the word `required`.

Sync properties
---------------
<< see "syncevolution --sync-property ?" >>

Datastore properties
--------------------
<< see "syncevolution --datastore-property ?" >>


EXAMPLES
========

List the known configuration templates::

   syncevolution --template ?

Create a new configuration, using the existing Memotoo template::

  syncevolution --configure \
                username=123456 \
                "password=!@#ABcd1234" \
                memotoo

Note that putting passwords into the command line, even for
short-lived processes as the one above, is a security risk in shared
environments, because the password is visible to everyone on the
machine. To avoid this, remove the password from the command above,
then add the password to the right config.ini file with a text editor.
This command shows the directory containing the file::

   syncevolution --print-configs

Review configuration::

   syncevolution --print-config memotoo

Synchronize all datastores::

  syncevolution memotoo

Deactivate all datastores::

  syncevolution --configure \
                sync=none \
                memotoo

Activate address book synchronization again, using the --sync shortcut::

  syncevolution --configure \
                --sync two-way \
                memotoo addressbook

Change the password for a configuration::

  syncevolution --configure \
                password=foo \
                memotoo

Set up another configuration for under a different account, using
the same default databases as above::

  syncevolution --configure \
                username=joe \
                password=foo \
                --template memotoo \
                memotoo_joe

Set up another configuration using the same account, but different
local databases (can be used to simulate synchronizing between two
clients, see `Exchanging Data`_::

  syncevolution --configure \
                username=123456 \
                password=!@#ABcd1234" \
                sync=none \
                memotoo@other
  
  syncevolution --configure \
                database=<name of other address book> \
                @other addressbook

  syncevolution --configure \
                sync=two-way \
                memotoo@other addressbook

  syncevolution memotoo 
  syncevolution memotoo@other

Migrate a configuration from the <= 0.7 format to the current one
and/or updates the configuration so that it looks like configurations
created anew with the current syncevolution::

  syncevolution --migrate memotoo


.. _local sync:

Synchronization beyond SyncML
=============================

In the simple examples above, SyncEvolution exchanges data with
servers via the SyncML protocol. Starting with release 1.2,
SyncEvolution also supports other protocols like CalDAV and
CardDAV.

These protocols are implemented in backends which behave like local
datastores. SyncEvolution then synchronizes data between a pair of
backends. Because the entire sync logic (matching of items, merging)
is done locally by SyncEvolution, this mode of operation is called
*local sync*.

Some examples of things that can be done with local sync:

* synchronize events with a CalDAV server and contacts with a CardDAV server
* mirror a local database as items in a directory, with format conversion
  and one-way or two-way data transfer (export vs. true syncing)

Because local sync involves two sides, two sync configurations are
needed. One is called the *target config*. Traditionally, this really
was a configuration called ``target-config``, for example
``target-config@google``. Using this particular name is no longer required.

The target config can hold properties which apply to all datastores
inside its context, like user name, password and URL for the server
(more on that below) and sync settings (like logging and data
backups). Once configured, the target config can be used to
list/import/export/update items via the SyncEvolution command line. It
cannot be used for synchronization because it does not defined what
the items are supposed to be synchronized with.

For synchronization, a second *originating config* is needed. This config has
the same role as the traditional SyncML sync configs and is typically
defined in the same implicit ``@default`` context as those
configs. All configs in that context use the same local data, thus turning
that local data into the hub through with data flows to all peers that the
host is configured to sync with.

A sync config becomes an originating config in a local sync by setting
the ``syncURL`` to the special URL ``local://[<target config
name>][@<some context name>]``. This selects the target config to
sync with. If the target config name is left out, the actual string
``target-config`` is used as name. The context can be omitted if the
target config name is unique. Originating and target config can be in
the same context. Care must be taken to not use a datastore more than
once in a local sync.

In addition, ``peerIsClient=1`` must be set in the originating config,
because SyncEvolution only supports running the SyncML client on the
target side. It makes sense to use the local databases on
originating side, because that side requires more frequent access to
the data.

The originating config defines the database pairs, either implicitly
(by using the same datastore names on both sides, which is possible when
different contexts are used) or explicitly (via the `uri` properties
set for the datastores on the originating side). The originating config
also defines the ``sync`` mode for each pair. ``uri`` and ``sync``
values on the target side are ignored and do not have to be specified.

As a special case, datastores used in combination with the target config
may access the credentials and ``syncURL`` stored there as fallback when
nothing was specified for the datastores directly. This makes sense for
the WebDAV and ActiveSync backends where the credentials are typically
the same and (depending on the web server) the same start URL can be
used to find calendar and contact databases.

  **Warning:** when setting password for the target config and using a
  keyring, a ``syncURL`` or a unique ``remoteDeviceID`` string must be
  set, because they are needed to identify the host in the keyring.

If this feature is not used, the ``syncURL`` could be left empty because
local sync itself does not use it. However, the command line expects
it to be set to ``none`` explicitly to detect typos.

  **Warning:** because the client in the local sync starts the sync,
  ``preventSlowSync=0`` must be set in the target config to have an effect.

TODO: this is inconsistent. Should we allow the user to set preventSlowSync
in the originating config and use that on the target side?


CalDAV and CardDAV
==================

This section explains how to use local syncing for CalDAV and
CardDAV. Both protocols are based on WebDAV and are provided by the
same backend. They share ``username/password/syncURL`` properties
defined in their target config.

The credentials must be provided if the server is password
protected. The ``syncURL`` is optional if the ``username`` is an email
address and the server supports auto-discovery of its CalDAV and/or
CardDAV services (using DNS SRV entries, ``.well-known`` URIs, properties
of the current principal, ...).

Alternatively, credentials can also be set in the ``databaseUser`` and
``databasePassword`` properties of the datastore. The downside is that these
values have to be set for each datastore and cannot be shared. The advantage
is that, in combination with setting ``database``, such datastores can be
used as part of a normal SyncML server or client sync config. SyncEvolution
then reads and writes data directly from the server and exchanges it
via SyncML with the peer that is defined in the sync config.

The ``database`` property of each datastore can be set to the URL of a
specific *collection* (= database in WebDAV terminology). If not set,
then the WebDAV backend first locates the server based on ``username``
or ``syncURL`` and then scans it for the default event resp. contact
collection. This is done once in the initial synchronization. At the end
of a successful synchroniation, the automatic choice is made permanent
by setting the ``database`` property.

  **Warning:** the protocols do not uniquely identify this default
  collection. The backend tries to make an educated guess, but it might
  pick the wrong one if the server provides more than one address book
  or calendar. It is safer to scan for collections manually with
  ``--print-databases`` and then use the URL of the desired collection
  as value of ``database``.

To scan for collections, use::

   syncevolution --print-databases \
                 backend=<caldav or carddav> \
                 username=<email address or user name> \
                 "password=!@#ABcd1234" \
                 syncURL=<base URL of server, if server auto-discovery is not supported>

Configuration templates for Google Calendar, Yahoo Calendar and a
generic CalDAV/CardDAV server are included in SyncEvolution. The Yahoo
template also contains an entry for contact synchronization, but using
it is not recommended due to known server-side issues.

The following commands set up synchronization with a generic WebDAV
server that supports CalDAV, CardDAV and scanning starting at the
root of the server.

TODO: document how to use Google.
TODO: Yahoo not currently supported, remove template?

For Google there is no common start URL for CalDAV and CardDAV. ::

   # configure target config
   syncevolution --configure \
                --template webdav \
                syncURL=http://example.com \
                username=123456 \
                "password=!@#ABcd1234" \
                target-config@webdav

   # configure sync config
   syncevolution --configure \
                 --template SyncEvolution_Client \
                 syncURL=local://@webdav \
                 username= \
                 password= \
                 webdav \
                 calendar addressbook

   # initial slow sync
   syncevolution --sync slow webdav

   # incremental sync
   syncevolution webdav

Here are some alternative ways of configuring the target config::

   # A) Server supports DNS auto-discovery via domain name in the username.
   syncevolution --configure \
                --template webdav \
                username=123456@example.com \
                "password=!@#ABcd1234" \
                target-config@webdav

   # B) Explicitly specify collections (from server documentation or --print-databases).
   #    The 'calendar' and 'addressbook' names are the ones expected by the sync config
   #    above, additional datastores can also be configured and/or the names can be changed.
   syncevolution --configure \
                username=123456 \
                "password=!@#ABcd1234" \
                --template none \
                syncURL=http://example.com \
                addressbook/backend=carddav \
                addressbook/database=http://example.com/addressbooks/123456/ \
                calendar/backend=caldav \
                calendar/database=http://example.com/calendar/123456/ \
                target-config@webdav \
                calendar addressbook

When creating these target configs, the command line tool tries to
verify that the datastores really work and (in the case of --template
webdav) will enable only datastores which really work. This involves
contacting the WebDAV server.

Finally, here is how the ``@webdav`` context needs to be configured so that SyncML
clients or servers can be added to it::

   # configure datastores
   syncevolution --configure \
                databaseUser=123456 \
                "databasePassword=!@#ABcd1234" \
                addressbook/backend=carddav \
                addressbook/database=http://example.com/addressbooks/123456/ \
                calendar/backend=caldav \
                calendar/database=http://example.com/calendar/123456/ \
                @webdav \
                calendar addressbook

   # configure one peer (Memotoo in this example):
   syncevolution --configure \
                 username=654321 \
                 password=^749@2524 \
                 memotoo@webdav

   # sync
   syncevolution --sync slow memotoo@webdav


NOTES
=====

Exchanging Data
---------------

SyncEvolution transmits address book entries as vCard 2.1 or 3.0
depending on the sync format chosen in the configuration. Evolution uses
3.0 internally, so SyncEvolution converts between the two formats as
needed. Calendar items and tasks can be sent and received in iCalendar
2.0 as well as vCalendar 1.0, but vCalendar 1.0 should be avoided if
possible because it cannot represent all data that Evolution stores.

.. note:: The Evolution backends are mentioned as examples;
   the same applies to other datastores.

How the server stores the items depends on its implementation and
configuration. To check which data is preserved, one can use this
procedure (described for contacts, but works the same way for
calendars and tasks):

1. synchronize the address book with the server
2. create a new address book in Evolution and view it in Evolution
   once (the second step is necessary in at least Evolution 2.0.4
   to make the new address book usable in SyncEvolution)
3. add a configuration for that second address book and the
   same URI on the SyncML server, see EXAMPLES_ above
4. synchronize again, this time using the other datastore

Now one can either compare the address books in Evolution or do that
automatically, described here for contacts:

- save the complete address books: mark all entries, save as vCard
- invoke `synccompare` with two file names as arguments and it will
  normalize and compare them automatically

Normalizing is necessary because the order of cards and their
properties as well as other minor formatting aspects may be
different. The output comes from a side-by-side comparison, but
is augmented by the script so that the context of each change
is always the complete item that was modified. Lines or items
following a ">" on the right side were added, those on the
left side followed by a "<" were removed, and those with
a "|" between text on the left and right side were modified.

The automatic unit testing (see HACKING) contains a `testItems`
test which verifies the copying of special entries using the
same method.

Modifying one of the address books or even both at the same time and
then synchronizing back and forth can be used to verify that
SyncEvolution works as expected. If you do not trust SyncEvolution or
the server, then it is prudent to run these checks with a copy of the
original address book. Make a backup of the .evolution/addressbook
directory.

Item Changes and Data Changes
-----------------------------

SyncML clients and servers consider each entry in a database as one
item. Items can be added, removed or updated. This is the item change
information that client and server exchange during a normal,
incremental synchronization.

If an item is saved, removed locally, and reimported, then this is
usually reported to a peer as "one item removed, one added" because
the information available to SyncEvolution is not sufficient to
determine that this is in fact the same item. One exception are
iCalendar 2.0 items with their globally unique ID: the modification
above will be reported to the server as "one item updated".

That is better, but still not quite correct because the content of the
item has not changed, only the meta information about it which is used
to detect changes. This cannot be avoided without creating additional
overhead for normal synchronizations.

SyncEvolution reports *item changes* (the number of added, removed and
updated items) as well as *data changes*. These data changes are
calculated by comparing database dumps using the `synccompare` tool.
Because this data comparison ignores information about which data
belongs to which item, it is able to detect that re-adding an item
that was removed earlier does not change the data, in contrast to the
item changes. On the other hand, removing one item and adding a
different one may look like updating just one item.

Automatic Backups and Logging
-----------------------------

To support recovery from a synchronization which damaged the
local data or modified it in an unexpected way, SyncEvolution
can create the following files during a synchronization:

- a dump of the data in a format which can be restored by
  SyncEvolution, usually a single file per item containing
  in a standard text format (VCARD/VCALENDAR)
- a full log file with debug information
- another dump of the data after the synchronization for
  automatic comparison of the before/after state with
  `synccompare`

If the sync configuration property ``logdir`` is set, then
a new directory will be created for each synchronization
in that directory, using the format `<peer>-<yyyy>-<mm>-<dd>-<hh>-<mm>[-<seq>]`
with the various fields filled in with the time when the
synchronization started. The sequence suffix will only be
used when necessary to make the name unique. By default,
SyncEvolution will never delete any data in that log
directory unless explicitly asked to keep only a limited
number of previous log directories.

This is done by setting the ``maxlogdirs`` limit to something
different than the empty string and 0. If a limit is set,
then SyncEvolution will only keep that many log directories
and start removing the "less interesting" ones when it reaches
the limit. Less interesting are those where no data changed
and no error occurred.

To avoid writing any additional log file or database dumps during
a synchronization, the ``logdir`` can be set to ``none``. To reduce
the verbosity of the log, set ``loglevel``. If not set or 0, then
the verbosity is set to 3 = DEBUG when writing to a log file and
2 = INFO when writing to the console directly. To debug issues
involving data conversion, level 4 also dumps the content of
items into the log.

ENVIRONMENT
===========

The following environment variables control where SyncEvolution finds
files and other aspects of its operations.

http_proxy
   Overrides the proxy settings temporarily. Setting it to an empty value
   disables the normal proxy settings.

HOME/XDG_CACHE_HOME/XDG_CONFIG_HOME
   SyncEvolution follows the XDG_ desktop standard for its files. By default,
   `$HOME/.config/syncevolution` is the location for configuration files.
   `$HOME/.cache/syncevolution` holds session directories with log files and
   database dumps.

.. _XDG: http://standards.freedesktop.org/basedir-spec/basedir-spec-latest.html

SYNCEVOLUTION_DEBUG
   Setting this to any value disables the filtering of stdout and stderr
   that SyncEvolution employs to keep noise from system libraries out
   of the command line output.

SYNCEVOLUTION_GNUTLS_DEBUG
   Enables additional debugging output when using the libsoup HTTP transport library.

SYNCEVOLUTION_DATA_DIR
   Overrides the default path to the bluetooth device lookup table,
   normally `/usr/lib/syncevolution/`.

SYNCEVOLUTION_BACKEND_DIR
   Overrides the default path to plugins, normally `/usr/lib/syncevolution/backends`.

SYNCEVOLUTION_LIBEXEC_DIR
   Overrides the path where additional helper executables are found, normally
   `/usr/libexec`.

SYNCEVOLUTION_LOCALE_DIR
   Overrides the path to directories with the different translations,
   normally `/usr/share/locale`.

SYNCEVOLUTION_TEMPLATE_DIR
   Overrides the default path to template files, normally
   `/usr/share/syncevolution/templates`.

SYNCEVOLUTION_XML_CONFIG_DIR
   Overrides the default path to the Synthesis XML configuration files, normally
   `/usr/share/syncevolution/xml`. These files are merged into one configuration
   each time the Synthesis SyncML engine is started as part of a sync session.

   Note that in addition to this directory, SyncEvolution also always
   searches for configuration files inside `$HOME/.config/syncevolution-xml`.
   Files with the same relative path and name as in `/usr/share/syncevolution/xml`
   override those files, others extend the final configuration.

BUGS
====

See `known issues`_ and the `support`_ web page for more information. 

.. _known issues: http://syncevolution.org/documentation/known-issues
.. _support: http://syncevolution.org/support

SEE ALSO
========

http://syncevolution.org

AUTHORS
=======

:Main developer:
     Patrick Ohly <patrick.ohly@intel.com>, http://www.estamos.de
:Contributors:
     http://syncevolution.org/about/contributors
:To contact the project publicly (preferred):
     syncevolution@syncevolution.org
:Intel-internal team mailing list (confidential):
     syncevolution@lists.intel.com
