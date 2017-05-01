/*
 * repmgr-action-standby.c
 *
 * Implements standby actions for the repmgr command line utility
 *
 * Copyright (c) 2ndQuadrant, 2010-2017
 */

#include "repmgr.h"
#include "dirutil.h"
#include "compat.h"

#include "repmgr-client-global.h"
#include "repmgr-action-standby.h"

static PGconn  *primary_conn = NULL;
static PGconn  *source_conn = NULL;

static int		server_version_num = UNKNOWN_SERVER_VERSION_NUM;

static char		local_data_directory[MAXPGPATH];
static bool		local_data_directory_provided = false;

static bool		upstream_record_found = false;
static int	    upstream_node_id = UNKNOWN_NODE_ID;
static char		upstream_data_directory[MAXPGPATH];

static t_conninfo_param_list recovery_conninfo;
static char		recovery_conninfo_str[MAXLEN];

static t_configfile_list config_files = T_CONFIGFILE_LIST_INITIALIZER;

static standy_clone_mode mode;

/* used by barman mode */
static char		local_repmgr_tmp_directory[MAXPGPATH];


static void check_barman_config(void);
static void	check_source_server(void);
static void	check_source_server_via_barman(void);


static void initialise_direct_clone(void);
static void config_file_list_init(t_configfile_list *list, int max_size);
static void config_file_list_add(t_configfile_list *list, const char *file, const char *filename, bool in_data_dir);

static void get_barman_property(char *dst, char *name, char *local_repmgr_directory);
static char *make_barman_ssh_command(char *buf);


void
do_standby_clone(void)
{

	/*
	 * conninfo params for the actual upstream node (which might be different
	 * to the node we're cloning from) to write to recovery.conf
	 */

	/*
	 * detecting the cloning mode
	 */
	mode = get_standby_clone_mode();

	/*
	 * In rsync mode, we need to check the SSH connection early
	 */
	if (mode == rsync)
	{
		int r;

		r = test_ssh_connection(runtime_options.host, runtime_options.remote_user);
		if (r != 0)
		{
			log_error(_("remote host %s is not reachable via SSH"),
					  runtime_options.host);
			exit(ERR_BAD_SSH);
		}
	}

	/*
	 * If dest_dir (-D/--pgdata) was provided, this will become the new data
	 * directory (otherwise repmgr will default to using the same directory
	 * path as on the source host).
	 *
	 * Note that barman mode requires -D/--pgdata.
	 *
	 * If -D/--pgdata is not supplied, and we're not cloning from barman,
	 * the source host's data directory will be fetched later, after
	 * we've connected to it.
	 */
	if (runtime_options.data_dir[0])
	{
		local_data_directory_provided = true;
		log_notice(_("destination directory '%s' provided"),
				   runtime_options.data_dir);
	}
	else if (mode == barman)
	{
		log_error(_("Barman mode requires a data directory"));
		log_hint(_("use -D/--pgdata to explicitly specify a data directory"));
		exit(ERR_BAD_CONFIG);
	}

	/* Sanity-check barman connection and installation */
	if (mode == barman)
	{
		/* this will exit with ERR_BARMAN if problems found */
		check_barman_config();
	}


	/*
	 * target directory (-D/--pgdata) provided - use that as new data directory
	 * (useful when executing backup on local machine only or creating the backup
	 * in a different local directory when backup source is a remote host)
	 */
	if (local_data_directory_provided == true)
	{
		strncpy(local_data_directory, runtime_options.data_dir, MAXPGPATH);
	}

	/*
	 * Initialise list of conninfo parameters which will later be used
	 * to create the `primary_conninfo` string in recovery.conf .
	 *
	 * We'll initialise it with the default values as seen by libpq,
	 * and overwrite them with the host settings specified on the command
	 * line. As it's possible the standby will be cloned from a node different
	 * to its intended upstream, we'll later attempt to fetch the
	 * upstream node record and overwrite the values set here with
	 * those from the upstream node record (excluding that record's
	 * application_name)
	 */
	initialize_conninfo_params(&recovery_conninfo, true);

	copy_conninfo_params(&recovery_conninfo, &source_conninfo);

	/*
	 * If application_name is set in repmgr.conf's conninfo parameter, use
	 * this value (if the source host was provided as a conninfo string, any
	 * application_name values set there will be overridden; we assume the only
	 * reason to pass an application_name via the command line is in the
	 * rare corner case where a user wishes to clone a server without
	 * providing repmgr.conf)
	 */
	if (strlen(config_file_options.conninfo))
	{
		char application_name[MAXLEN] = "";

		get_conninfo_value(config_file_options.conninfo, "application_name", application_name);
		if (strlen(application_name))
		{
			param_set(&recovery_conninfo, "application_name", application_name);
		}
	}


	/*
	 * --upstream-conninfo supplied, which we interpret to imply
	 * --no-upstream-connection as well - the use case for this option is when
	 * the upstream is not available, so no point in checking for it.
	 */

	if (*runtime_options.upstream_conninfo)
		runtime_options.no_upstream_connection = true;

	/* By default attempt to connect to the source server */
	if (runtime_options.no_upstream_connection == false)
	{
		check_source_server();
	}

	if (mode == barman && PQstatus(source_conn) != CONNECTION_OK)
	{
		/*
		 * Here we don't have a connection to the upstream node, and are executing
		 * in Barman mode - we can try and connect via the Barman server to extract
		 * the upstream node's conninfo string.
		 *
		 * To do this we need to extract Barman's conninfo string, replace the database
		 * name with the repmgr one (they could well be different) and remotely execute
		 * psql.
		 */
		check_source_server_via_barman();
	}

	if (upstream_record_found == true)
	{
		/*  parse returned upstream conninfo string to recovery primary_conninfo params*/
		char	   *errmsg = NULL;
		bool	    parse_success;

		log_verbose(LOG_DEBUG, "parsing upstream conninfo string \"%s\"", recovery_conninfo_str);

		/* parse_conninfo_string() here will remove the upstream's `application_name`, if set */

		parse_success = parse_conninfo_string(recovery_conninfo_str, &recovery_conninfo, errmsg, true);
		if (parse_success == false)
		{
			log_error(_("unable to parse conninfo string \"%s\" for upstream node:\n%s"),
					  recovery_conninfo_str, errmsg);

			PQfinish(source_conn);
			exit(ERR_BAD_CONFIG);
		}
	}
	else
	{
		/*
		 * If no upstream node record found, we'll abort with an error here,
		 * unless -F/--force is used, in which case we'll use the parameters
		 * provided on the command line (and assume the user knows what they're
		 * doing).
		 */

		if (!runtime_options.force)
		{
			log_error(_("no record found for upstream node (upstream_node_id: %i)"),
					  upstream_node_id);
			log_hint(_("use -F/--force to create \"primary_conninfo\" based on command-line parameters"));
			PQfinish(source_conn);
			exit(ERR_BAD_CONFIG);
		}
	}

	/* If --replication-user was set, use that value for the primary_conninfo user */
	if (*runtime_options.replication_user)
	{
		param_set(&recovery_conninfo, "user", runtime_options.replication_user);
	}

	if (mode != barman)
	{
		initialise_direct_clone();
	}

}



void
check_barman_config(void)
{
	char		datadir_list_filename[MAXLEN];
	char		barman_command_buf[MAXLEN] = "";

	char		command[MAXLEN];
	bool		command_ok;

	/*
	 * Check that there is at least one valid backup
	 */

	log_info(_("connecting to Barman server to verify backup for %s"), config_file_options.barman_server);

	maxlen_snprintf(command, "%s show-backup %s latest > /dev/null",
					make_barman_ssh_command(barman_command_buf),
				 	config_file_options.barman_server);

	command_ok = local_command(command, NULL);

	if (command_ok == false)
	{
		log_error(_("no valid backup for server %s was found in the Barman catalogue"),
				  config_file_options.barman_server);
		log_hint(_("refer to the Barman documentation for more information\n"));

		exit(ERR_BARMAN);
	}

	/*
	 * Create the local repmgr subdirectory
	 */

	maxlen_snprintf(local_repmgr_tmp_directory,
					"%s/repmgr",  local_data_directory);

	maxlen_snprintf(datadir_list_filename,
					"%s/data.txt", local_repmgr_tmp_directory);

	if (!create_pg_dir(local_data_directory, runtime_options.force))
	{
		log_error(_("unable to use directory %s"),
				local_data_directory);
		log_hint(_("use -F/--force option to force this directory to be overwritten\n"));
			exit(ERR_BAD_CONFIG);
	}

	if (!create_pg_dir(local_repmgr_tmp_directory, runtime_options.force))
	{
		log_error(_("unable to create directory \"%s\""),
				  local_repmgr_tmp_directory);

		exit(ERR_BAD_CONFIG);
	}

	/*
	 * Fetch server parameters from Barman
	 */
	log_info(_("connecting to Barman server to fetch server parameters"));

	maxlen_snprintf(command, "%s show-server %s > %s/show-server.txt",
					make_barman_ssh_command(barman_command_buf),
					config_file_options.barman_server,
					local_repmgr_tmp_directory);

	command_ok = local_command(command, NULL);

	if (command_ok == false)
	{
		log_error(_("unable to fetch server parameters from Barman server"));

		exit(ERR_BARMAN);
	}

}



static void
check_source_server()
{
	char		cluster_size[MAXLEN];
	t_node_info node_record = T_NODE_INFO_INITIALIZER;
	int			query_result;
	t_extension_status extension_status;

	/* Attempt to connect to the upstream server to verify its configuration */
	log_info(_("connecting to upstream node"));

	source_conn = establish_db_connection_by_params((const char**)source_conninfo.keywords,
													(const char**)source_conninfo.values,
													false);

	/*
	 * Unless in barman mode, exit with an error;
	 * establish_db_connection_by_params() will have already logged an error message
	 */
	if (PQstatus(source_conn) != CONNECTION_OK)
	{
		PQfinish(source_conn);

		if (mode == barman)
			return;
		else
			exit(ERR_DB_CON);
	}

	/*
	 * If a connection was established, perform some sanity checks on the
	 * provided upstream connection
	 */


	/* Verify that upstream node is a supported server version */
	log_verbose(LOG_INFO, _("connected to source node, checking its state"));

	server_version_num = check_server_version(source_conn, "master", true, NULL);

	check_upstream_config(source_conn, server_version_num, true);

	if (get_cluster_size(source_conn, cluster_size) == false)
		exit(ERR_DB_QUERY);

	log_info(_("successfully connected to source node"));
	log_detail(_("current installation size is %s"),
			   cluster_size);

	/*
	 * If --recovery-min-apply-delay was passed, check that
	 * we're connected to PostgreSQL 9.4 or later
	 */
	// XXX should this be a config file parameter?
	if (*runtime_options.recovery_min_apply_delay)
	{
		if (server_version_num < 90400)
		{
			log_error(_("PostgreSQL 9.4 or greater required for --recovery-min-apply-delay\n"));
			PQfinish(source_conn);
			exit(ERR_BAD_CONFIG);
		}
	}

	/*
	 * If the upstream node is a standby, try to connect to the primary too so we
	 * can write an event record
	 */
	if (is_standby(source_conn))
	{
		primary_conn = get_master_connection(source_conn, NULL, NULL);

		// XXX check this worked?
	}
	else
	{
		primary_conn = source_conn;
	}

	/*
	 * Sanity-check that the master node has a repmgr schema - if not
	 * present, fail with an error unless -F/--force is used (to enable
	 * repmgr to be used as a standalone clone tool)
	 */

	extension_status = get_repmgr_extension_status(primary_conn);

	if (extension_status != REPMGR_INSTALLED)
	{
		if (!runtime_options.force)
		{
			/* schema doesn't exist */
			log_error(_("repmgr extension not found on source node"));
			log_hint(_("check that the upstream server is part of a repmgr cluster"));
			PQfinish(source_conn);
			exit(ERR_BAD_CONFIG);
		}

		log_warning(_("repmgr extension not found on source node"));
	}

	/* Fetch the source's data directory */
	if (get_pg_setting(source_conn, "data_directory", upstream_data_directory) == false)
	{
		log_error(_("unable to retrieve source node's data directory"));
		log_hint(_("STANDBY CLONE must be run as a database superuser"));
		PQfinish(source_conn);
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * If no target data directory was explicitly provided, we'll default to
	 * the source host's data directory.
	 */
	if (local_data_directory_provided == false)
	{
		strncpy(local_data_directory, upstream_data_directory, MAXPGPATH);

		log_notice(_("setting data directory to: \"%s\""), local_data_directory);
		log_hint(_("use -D/--pgdata to explicitly specify a data directory"));
	}

	/*
	 * Copy the source connection so that we have some default values,
	 * particularly stuff like passwords extracted from PGPASSFILE;
	 * these will be overridden from the upstream conninfo, if provided.
	 *
	 * XXX only allow passwords if --use-conninfo-password
	 */
	conn_to_param_list(source_conn, &recovery_conninfo);

	/*
	 * Attempt to find the upstream node record
	 */
	if (config_file_options.upstream_node_id == NO_UPSTREAM_NODE)
		upstream_node_id = get_master_node_id(source_conn);
	else
		upstream_node_id = config_file_options.upstream_node_id;

	query_result = get_node_record(source_conn, upstream_node_id, &node_record);

	if (query_result)
	{
		upstream_record_found = true;
		strncpy(recovery_conninfo_str, node_record.conninfo, MAXLEN);
	}

	/*
	 * check that there's no existing node record with the same name but
	 * different ID
	 */
	query_result = get_node_record_by_name(source_conn, config_file_options.node_name, &node_record);

	if (query_result)
	{
		log_error(_("another node (node_id: %i) already exists with node_name \"%s\""),
				  node_record.node_id,
				  config_file_options.node_name);
		PQfinish(source_conn);
		exit(ERR_BAD_CONFIG);
	}

}


static void
check_source_server_via_barman()
{
	char		    buf[MAXLEN];
	char		    barman_conninfo_str[MAXLEN];
	t_conninfo_param_list barman_conninfo;
	char		   *errmsg = NULL;
	bool		    parse_success,
				    command_success;
	char		    where_condition[MAXLEN];
	PQExpBufferData command_output;
	PQExpBufferData repmgr_conninfo_buf;

	int c;

	get_barman_property(barman_conninfo_str, "conninfo", local_repmgr_tmp_directory);

	initialize_conninfo_params(&barman_conninfo, false);

	/* parse_conninfo_string() here will remove the upstream's `application_name`, if set */
	parse_success = parse_conninfo_string(barman_conninfo_str, &barman_conninfo, errmsg, true);

	if (parse_success == false)
	{
		log_error(_("Unable to parse barman conninfo string \"%s\":\n%s"),
				barman_conninfo_str, errmsg);
		exit(ERR_BARMAN);
	}

	/* Overwrite database name in the parsed parameter list */
	param_set(&barman_conninfo, "dbname", runtime_options.dbname);

	/* Rebuild the Barman conninfo string */
	initPQExpBuffer(&repmgr_conninfo_buf);

	for (c = 0; c < barman_conninfo.size && barman_conninfo.keywords[c] != NULL; c++)
	{
		if (repmgr_conninfo_buf.len != 0)
			appendPQExpBufferChar(&repmgr_conninfo_buf, ' ');

		appendPQExpBuffer(&repmgr_conninfo_buf, "%s=",
						  barman_conninfo.keywords[c]);
		appendConnStrVal(&repmgr_conninfo_buf,
						 barman_conninfo.values[c]);
	}

	log_verbose(LOG_DEBUG,
				"repmgr database conninfo string on barman server: %s",
				repmgr_conninfo_buf.data);

	// XXX check this works in all cases
	maxlen_snprintf(where_condition, "node_id=%i", upstream_node_id);

	initPQExpBuffer(&command_output);
	maxlen_snprintf(buf,
					"ssh %s \"psql -Aqt \\\"%s\\\" -c \\\""
					" SELECT conninfo"
					" FROM repmgr.nodes"
					" WHERE %s"
					" AND active IS TRUE"
					"\\\"\"",
					config_file_options.barman_host,
					repmgr_conninfo_buf.data,
					where_condition);

	termPQExpBuffer(&repmgr_conninfo_buf);

	command_success = local_command(buf, &command_output);

	if (command_success == false)
	{
		log_error(_("unable to execute database query via Barman server"));
		exit(ERR_BARMAN);
	}

	maxlen_snprintf(recovery_conninfo_str, "%s", command_output.data);
	string_remove_trailing_newlines(recovery_conninfo_str);

	upstream_record_found = true;
	log_verbose(LOG_DEBUG,
				"upstream node conninfo string extracted via barman server: %s",
				recovery_conninfo_str);

	termPQExpBuffer(&command_output);
}



/*
 * In pg_basebackup/rsync modes, configure the target data directory
 * if necessary, and fetch information about tablespaces and configuration
 * files.
 */
static void
initialise_direct_clone(void)
{
	PGresult *res;
	int i;

	PQExpBufferData	  query;

	/*
	 * Check the destination data directory can be used
	 * (in Barman mode, this directory will already have been created)
	 */

	if (!create_pg_dir(local_data_directory, runtime_options.force))
	{
		log_error(_("unable to use directory %s ..."),
				  local_data_directory);
		log_hint(_("use -F/--force to force this directory to be overwritten\n"));
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * Check that tablespaces named in any `tablespace_mapping` configuration
	 * file parameters exist.
	 *
	 * pg_basebackup doesn't verify mappings, so any errors will not be caught.
	 * We'll do that here as a value-added service.
	 *
	 * -T/--tablespace-mapping is not available as a pg_basebackup option for
	 * PostgreSQL 9.3 - we can only handle that with rsync, so if `--rsync-only`
	 * not set, fail with an error
	 */

	if (config_file_options.tablespace_mapping.head != NULL)
	{
		TablespaceListCell *cell;

		if (server_version_num < 90400 && !runtime_options.rsync_only)
		{
			log_error(_("in PostgreSQL 9.3, tablespace mapping can only be used in conjunction with --rsync-only"));
			PQfinish(source_conn);
			exit(ERR_BAD_CONFIG);
		}

		for (cell = config_file_options.tablespace_mapping.head; cell; cell = cell->next)
		{
			initPQExpBuffer(&query);

			// XXX escape value
			appendPQExpBuffer(&query,
							  "SELECT spcname "
							  "  FROM pg_catalog.pg_tablespace "
							  " WHERE pg_tablespace_location(oid) = '%s'",
							  cell->old_dir);
			res = PQexec(source_conn, query.data);

			termPQExpBuffer(&query);

			if (PQresultStatus(res) != PGRES_TUPLES_OK)
			{
				log_error(_("unable to execute tablespace query:\n  %s"),
						  PQerrorMessage(source_conn));
				PQclear(res);
				PQfinish(source_conn);
				exit(ERR_BAD_CONFIG);
			}

			/* TODO: collate errors and output at end of loop */
			if (PQntuples(res) == 0)
			{
				log_error(_("no tablespace matching path '%s' found\n"),
						  cell->old_dir);
				PQclear(res);
				PQfinish(source_conn);
				exit(ERR_BAD_CONFIG);
			}
		}
	}

	/*
	 * Obtain configuration file locations
	 * We'll check to see whether the configuration files are in the data
	 * directory - if not we'll have to copy them via SSH, if copying
	 * requested.
	 *
	 * XXX: if configuration files are symlinks to targets outside the data
	 * directory, they won't be copied by pg_basebackup, but we can't tell
	 * this from the below query; we'll probably need to add a check for their
	 * presence and if missing force copy by SSH
	 */

	initPQExpBuffer(&query);

	appendPQExpBuffer(&query,
					  "  WITH dd AS ( "
					  "    SELECT setting AS data_directory"
					  "      FROM pg_catalog.pg_settings "
					  "     WHERE name = 'data_directory' "
					  "  ) "
					  "    SELECT DISTINCT(sourcefile), "
					  "           regexp_replace(sourcefile, '^.*\\/', '') AS filename, "
					  "           sourcefile ~ ('^' || dd.data_directory) AS in_data_dir "
					  "      FROM dd, pg_catalog.pg_settings ps "
					  "     WHERE sourcefile IS NOT NULL "
					  "  ORDER BY 1 ");

	log_debug("standby clone: %s", query.data);
	res = PQexec(source_conn, query.data);
	termPQExpBuffer(&query);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_error(_("unable to retrieve configuration file locations:\n  %s"),
				  PQerrorMessage(source_conn));
		PQclear(res);
		PQfinish(source_conn);
		exit(ERR_BAD_CONFIG);
	}

	/*
	 * allocate memory for config file array - number of rows returned from
	 * above query + 2 for pg_hba.conf, pg_ident.conf
	 */

	config_file_list_init(&config_files, PQntuples(res) + 2);

	for (i = 0; i < PQntuples(res); i++)
	{
		config_file_list_add(&config_files,
							 PQgetvalue(res, i, 0),
							 PQgetvalue(res, i, 1),
							 strcmp(PQgetvalue(res, i, 2), "t") == 1 ? true : false);
	}

	PQclear(res);

	/* Fetch locations of pg_hba.conf and pg_ident.conf */
	initPQExpBuffer(&query);

	appendPQExpBuffer(&query,
					  "  WITH dd AS ( "
					  "    SELECT setting AS data_directory"
					  "      FROM pg_catalog.pg_settings "
					  "     WHERE name = 'data_directory' "
					  "  ) "
					  "    SELECT ps.setting, "
					  "           regexp_replace(setting, '^.*\\/', '') AS filename, "
					  "           ps.setting ~ ('^' || dd.data_directory) AS in_data_dir "
					  "      FROM dd, pg_catalog.pg_settings ps "
					  "     WHERE ps.name IN ('hba_file', 'ident_file') "
					  "  ORDER BY 1 ");

	log_debug("standby clone: %s", query.data);
	res = PQexec(source_conn, query.data);
	termPQExpBuffer(&query);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		log_error(_("unable to retrieve configuration file locations: %s\n"),
				  PQerrorMessage(source_conn));
		PQclear(res);
		PQfinish(source_conn);
		exit(ERR_BAD_CONFIG);
	}

	for (i = 0; i < PQntuples(res); i++)
	{
		config_file_list_add(&config_files,
							 PQgetvalue(res, i, 0),
							 PQgetvalue(res, i, 1),
							 strcmp(PQgetvalue(res, i, 2), "t") == 1 ? true : false);
	}

	PQclear(res);

	/*
	 * If replication slots requested, create appropriate slot on
	 * the primary; this must be done before pg_start_backup() is
	 * issued, either by us or by pg_basebackup.
	 *
	 * Replication slots are not supported (and not very useful
	 * anyway) in Barman mode.
	 */

	if (config_file_options.use_replication_slots)
	{
		PQExpBufferData event_details;
		initPQExpBuffer(&event_details);

		if (create_replication_slot(source_conn, repmgr_slot_name, server_version_num, &event_details) == false)
		{
			log_error("%s", event_details.data);

			create_event_record(primary_conn,
								&config_file_options,
								config_file_options.node_id,
								"standby_clone",
								false,
								event_details.data);

			PQfinish(source_conn);
			exit(ERR_DB_QUERY);
		}

		termPQExpBuffer(&event_details);

		log_notice(_("replication slot '%s' created on upstream node (node_id: %i)"),
				   repmgr_slot_name,
				   upstream_node_id);
	}

}


static char *
make_barman_ssh_command(char *buf)
{
	static char config_opt[MAXLEN] = "";

	if (strlen(config_file_options.barman_config))
		maxlen_snprintf(config_opt,
						" --config=%s",
						config_file_options.barman_config);

	maxlen_snprintf(buf,
					"ssh %s barman%s",
					config_file_options.barman_server,
					config_opt);

	return buf;
}


void
get_barman_property(char *dst, char *name, char *local_repmgr_directory)
{
	PQExpBufferData command_output;
	char buf[MAXLEN];
	char command[MAXLEN];
	char *p;

	initPQExpBuffer(&command_output);

	maxlen_snprintf(command,
					"grep \"^\t%s:\" %s/show-server.txt",
					name, local_repmgr_tmp_directory);
	(void)local_command(command, &command_output);

	maxlen_snprintf(buf, "\t%s: ", name);
	p = string_skip_prefix(buf, command_output.data);
	if (p == NULL)
	{
		log_error("unexpected output from Barman: %s",
				  command_output.data);
		exit(ERR_INTERNAL);
	}

	strncpy(dst, p, MAXLEN);
	string_remove_trailing_newlines(dst);

	termPQExpBuffer(&command_output);
}

static void
config_file_list_init(t_configfile_list *list, int max_size)
{
	list->size = max_size;
	list->entries = 0;
	list->files = pg_malloc0(sizeof(t_configfile_info *) * max_size);
}


static void
config_file_list_add(t_configfile_list *list, const char *file, const char *filename, bool in_data_dir)
{
	/* Failsafe to prevent entries being added beyond the end */
	if (list->entries == list->size)
		return;

	list->files[list->entries] = pg_malloc0(sizeof(t_configfile_info));


	strncpy(list->files[list->entries]->filepath, file, MAXPGPATH);
	canonicalize_path(list->files[list->entries]->filepath);


	strncpy(list->files[list->entries]->filename, filename, MAXPGPATH);
	list->files[list->entries]->in_data_directory = in_data_dir;

	list->entries ++;
}
