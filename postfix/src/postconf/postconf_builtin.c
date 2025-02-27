/*++
/* NAME
/*	postconf_builtin 3
/* SUMMARY
/*	built-in main.cf parameter support
/* SYNOPSIS
/*	#include <postconf.h>
/*
/*	void	pcf_register_builtin_parameters(procname, pid)
/*	const char *procname;
/*	pid_t	pid;
/* DESCRIPTION
/*	pcf_register_builtin_parameters() initializes the global
/*	main.cf parameter name space and adds all built-in parameter
/*	information.
/*
/*	Arguments:
/*.IP procname
/*	Provides the default value for the "process_name" parameter.
/*.IP pid
/*	Provides the default value for the "process_id" parameter.
/* DIAGNOSTICS
/*	Problems are reported to the standard error stream.
/* LICENSE
/* .ad
/* .fi
/*	The Secure Mailer license must be distributed with this software.
/* AUTHOR(S)
/*	Wietse Venema
/*	IBM T.J. Watson Research
/*	P.O. Box 704
/*	Yorktown Heights, NY 10598, USA
/*
/*	Wietse Venema
/*	Google, Inc.
/*	111 8th Avenue
/*	New York, NY 10011, USA
/*--*/

/* System library. */

#include <sys_defs.h>
#include <string.h>

#ifdef USE_PATHS_H
#include <paths.h>
#endif

/* Utility library. */

#include <msg.h>
#include <mymalloc.h>
#include <htable.h>
#include <vstring.h>
#include <get_hostname.h>
#include <stringops.h>

/* Global library. */

#include <mynetworks.h>
#include <mail_conf.h>
#include <mail_params.h>
#include <mail_version.h>
#include <mail_proto.h>
#include <mail_addr.h>
#include <inet_proto.h>
#include <server_acl.h>

/* Application-specific. */

#include <postconf.h>

 /*
  * Support for built-in parameters: declarations generated by scanning
  * actual C source files.
  */
#include "time_vars.h"
#include "bool_vars.h"
#include "int_vars.h"
#include "str_vars.h"
#include "raw_vars.h"
#include "nint_vars.h"
#include "nbool_vars.h"
#include "long_vars.h"

 /*
  * Support for built-in parameters: manually extracted.
  */
#include "install_vars.h"

 /*
  * Support for built-in parameters: lookup tables generated by scanning
  * actual C source files.
  */
static const CONFIG_TIME_TABLE pcf_time_table[] = {
#include "time_table.h"
    0,
};

static const CONFIG_BOOL_TABLE pcf_bool_table[] = {
#include "bool_table.h"
    0,
};

static const CONFIG_INT_TABLE pcf_int_table[] = {
#include "int_table.h"
    0,
};

static const CONFIG_STR_TABLE pcf_str_table[] = {
#include "str_table.h"
#include "install_table.h"
    0,
};

static const CONFIG_RAW_TABLE pcf_raw_table[] = {
#include "raw_table.h"
    0,
};

static const CONFIG_NINT_TABLE pcf_nint_table[] = {
#include "nint_table.h"
    0,
};

static const CONFIG_NBOOL_TABLE pcf_nbool_table[] = {
#include "nbool_table.h"
    0,
};

static const CONFIG_LONG_TABLE pcf_long_table[] = {
#include "long_table.h"
    0,
};

 /*
  * Legacy parameters for backwards compatibility.
  */
static const CONFIG_STR_TABLE pcf_legacy_str_table[] = {
    {"virtual_maps", ""},
    {"fallback_relay", ""},
    {"authorized_verp_clients", ""},
    {"smtpd_client_connection_limit_exceptions", ""},
    {"postscreen_dnsbl_ttl", ""},
    {"postscreen_blacklist_action", ""},
    {"postscreen_dnsbl_whitelist_threshold", ""},
    {"postscreen_whitelist_interfaces", ""},
    {"lmtp_per_record_deadline", ""},
    {"smtp_per_record_deadline", ""},
    {"smtpd_per_record_deadline", ""},
    0,
};

 /*
  * Parameters whose default values are normally initialized by calling a
  * function. We direct the calls to our own versions of those functions
  * because the run-time conditions are slightly different.
  * 
  * Important: if the evaluation of a parameter default value has any side
  * effects, then those side effects must happen only once.
  */
static const char *pcf_check_myhostname(void);
static const char *pcf_check_mydomainname(void);
static const char *pcf_mynetworks(void);

#include "str_fn_vars.h"

static const CONFIG_STR_FN_TABLE pcf_str_fn_table[] = {
#include "str_fn_table.h"
    0,
};

 /*
  * Parameters whose default values are normally initialized by ad-hoc code.
  * The AWK script cannot identify these parameters or values, so we provide
  * our own.
  * 
  * Important: if the evaluation of a parameter default value has any side
  * effects, then those side effects must happen only once.
  */
static CONFIG_STR_TABLE pcf_adhoc_procname = {VAR_PROCNAME};
static CONFIG_STR_TABLE pcf_adhoc_servname = {VAR_SERVNAME};
static CONFIG_INT_TABLE pcf_adhoc_pid = {VAR_PID};

#define STR(x) vstring_str(x)

/* pcf_check_myhostname - lookup hostname and validate */

static const char *pcf_check_myhostname(void)
{
    static const char *name;
    const char *dot;
    const char *domain;

    /*
     * Use cached result.
     */
    if (name)
	return (name);

    /*
     * If the local machine name is not in FQDN form, try to append the
     * contents of $mydomain.
     */
    name = get_hostname();
    if ((dot = strchr(name, '.')) == 0) {
	if ((domain = mail_conf_lookup_eval(VAR_MYDOMAIN)) == 0)
	    domain = DEF_MYDOMAIN;
	name = concatenate(name, ".", domain, (char *) 0);
    }
    return (name);
}

/* pcf_get_myhostname - look up and store my hostname */

static void pcf_get_myhostname(void)
{
    const char *name;

    if ((name = mail_conf_lookup_eval(VAR_MYHOSTNAME)) == 0)
	name = pcf_check_myhostname();
    var_myhostname = mystrdup(name);
}

/* pcf_check_mydomainname - lookup domain name and validate */

static const char *pcf_check_mydomainname(void)
{
    static const char *domain;
    char   *dot;

    /*
     * Use cached result.
     */
    if (domain)
	return (domain);

    /*
     * Use a default domain when the hostname is not a FQDN ("foo").
     */
    if (var_myhostname == 0)
	pcf_get_myhostname();
    if ((dot = strchr(var_myhostname, '.')) == 0)
	return (domain = DEF_MYDOMAIN);
    return (domain = mystrdup(dot + 1));
}

/* pcf_mynetworks - lookup network address list */

static const char *pcf_mynetworks(void)
{
    static const char *networks;
    const char *junk;

    /*
     * Use cached result.
     */
    if (networks)
	return (networks);

    if (var_inet_interfaces == 0) {
	if ((pcf_cmd_mode & PCF_SHOW_DEFS)
	    || (junk = mail_conf_lookup_eval(VAR_INET_INTERFACES)) == 0)
	    junk = pcf_expand_parameter_value((VSTRING *) 0, pcf_cmd_mode,
					      DEF_INET_INTERFACES,
					      (PCF_MASTER_ENT *) 0);
	var_inet_interfaces = mystrdup(junk);
    }
    if (var_mynetworks_style == 0) {
	if ((pcf_cmd_mode & PCF_SHOW_DEFS)
	    || (junk = mail_conf_lookup_eval(VAR_MYNETWORKS_STYLE)) == 0)
	    junk = pcf_expand_parameter_value((VSTRING *) 0, pcf_cmd_mode,
					      DEF_MYNETWORKS_STYLE,
					      (PCF_MASTER_ENT *) 0);
	var_mynetworks_style = mystrdup(junk);
    }
    if (var_inet_protocols == 0) {
	if ((pcf_cmd_mode & PCF_SHOW_DEFS)
	    || (junk = mail_conf_lookup_eval(VAR_INET_PROTOCOLS)) == 0)
	    junk = pcf_expand_parameter_value((VSTRING *) 0, pcf_cmd_mode,
					      DEF_INET_PROTOCOLS,
					      (PCF_MASTER_ENT *) 0);
	var_inet_protocols = mystrdup(junk);
	(void) inet_proto_init(VAR_INET_PROTOCOLS, var_inet_protocols);
    }
    return (networks = mystrdup(mynetworks()));
}

/* pcf_conv_bool_parameter - get boolean parameter string value */

static const char *pcf_conv_bool_parameter(void *ptr)
{
    CONFIG_BOOL_TABLE *cbt = (CONFIG_BOOL_TABLE *) ptr;

    return (cbt->defval ? "yes" : "no");
}

/* pcf_conv_time_parameter - get relative time parameter string value */

static const char *pcf_conv_time_parameter(void *ptr)
{
    CONFIG_TIME_TABLE *ctt = (CONFIG_TIME_TABLE *) ptr;

    return (ctt->defval);
}

/* pcf_conv_int_parameter - get integer parameter string value */

static const char *pcf_conv_int_parameter(void *ptr)
{
    CONFIG_INT_TABLE *cit = (CONFIG_INT_TABLE *) ptr;

    return (STR(vstring_sprintf(pcf_param_string_buf, "%d", cit->defval)));
}

/* pcf_conv_str_parameter - get string parameter string value */

static const char *pcf_conv_str_parameter(void *ptr)
{
    CONFIG_STR_TABLE *cst = (CONFIG_STR_TABLE *) ptr;

    return (cst->defval);
}

/* pcf_conv_str_fn_parameter - get string-function parameter string value */

static const char *pcf_conv_str_fn_parameter(void *ptr)
{
    CONFIG_STR_FN_TABLE *cft = (CONFIG_STR_FN_TABLE *) ptr;

    return (cft->defval());
}

/* pcf_conv_raw_parameter - get raw string parameter string value */

static const char *pcf_conv_raw_parameter(void *ptr)
{
    CONFIG_RAW_TABLE *rst = (CONFIG_RAW_TABLE *) ptr;

    return (rst->defval);
}

/* pcf_conv_nint_parameter - get new integer parameter string value */

static const char *pcf_conv_nint_parameter(void *ptr)
{
    CONFIG_NINT_TABLE *rst = (CONFIG_NINT_TABLE *) ptr;

    return (rst->defval);
}

/* pcf_conv_nbool_parameter - get new boolean parameter string value */

static const char *pcf_conv_nbool_parameter(void *ptr)
{
    CONFIG_NBOOL_TABLE *bst = (CONFIG_NBOOL_TABLE *) ptr;

    return (bst->defval);
}

/* pcf_conv_long_parameter - get long parameter string value */

static const char *pcf_conv_long_parameter(void *ptr)
{
    CONFIG_LONG_TABLE *clt = (CONFIG_LONG_TABLE *) ptr;

    return (STR(vstring_sprintf(pcf_param_string_buf, "%ld", clt->defval)));
}

/* pcf_register_builtin_parameters - add built-ins to the global name space */

void    pcf_register_builtin_parameters(const char *procname, pid_t pid)
{
    const char *myname = "pcf_register_builtin_parameters";
    const CONFIG_TIME_TABLE *ctt;
    const CONFIG_BOOL_TABLE *cbt;
    const CONFIG_INT_TABLE *cit;
    const CONFIG_STR_TABLE *cst;
    const CONFIG_STR_FN_TABLE *cft;
    const CONFIG_RAW_TABLE *rst;
    const CONFIG_NINT_TABLE *nst;
    const CONFIG_NBOOL_TABLE *bst;
    const CONFIG_LONG_TABLE *lst;

    /*
     * Sanity checks.
     */
    if (pcf_param_table != 0)
	msg_panic("%s: global parameter table is already initialized", myname);

    /*
     * Initialize the global parameter table.
     */
    pcf_param_table = PCF_PARAM_TABLE_CREATE(1000);

    /*
     * Add the built-in parameters to the global name space. The class
     * (built-in) is tentative; some parameters are actually service-defined,
     * but they have their own default value.
     */
    for (ctt = pcf_time_table; ctt->name; ctt++)
	PCF_PARAM_TABLE_ENTER(pcf_param_table, ctt->name,
			      PCF_PARAM_FLAG_BUILTIN, (void *) ctt,
			      pcf_conv_time_parameter);
    for (cbt = pcf_bool_table; cbt->name; cbt++)
	PCF_PARAM_TABLE_ENTER(pcf_param_table, cbt->name,
			      PCF_PARAM_FLAG_BUILTIN, (void *) cbt,
			      pcf_conv_bool_parameter);
    for (cit = pcf_int_table; cit->name; cit++)
	PCF_PARAM_TABLE_ENTER(pcf_param_table, cit->name,
			      PCF_PARAM_FLAG_BUILTIN, (void *) cit,
			      pcf_conv_int_parameter);
    for (cst = pcf_str_table; cst->name; cst++)
	PCF_PARAM_TABLE_ENTER(pcf_param_table, cst->name,
			      PCF_PARAM_FLAG_BUILTIN, (void *) cst,
			      pcf_conv_str_parameter);
    for (cft = pcf_str_fn_table; cft->name; cft++)
	PCF_PARAM_TABLE_ENTER(pcf_param_table, cft->name,
			      PCF_PARAM_FLAG_BUILTIN, (void *) cft,
			      pcf_conv_str_fn_parameter);
    for (rst = pcf_raw_table; rst->name; rst++)
	PCF_PARAM_TABLE_ENTER(pcf_param_table, rst->name,
			      PCF_PARAM_FLAG_BUILTIN | PCF_PARAM_FLAG_RAW,
			      (void *) rst, pcf_conv_raw_parameter);
    for (nst = pcf_nint_table; nst->name; nst++)
	PCF_PARAM_TABLE_ENTER(pcf_param_table, nst->name,
			      PCF_PARAM_FLAG_BUILTIN, (void *) nst,
			      pcf_conv_nint_parameter);
    for (bst = pcf_nbool_table; bst->name; bst++)
	PCF_PARAM_TABLE_ENTER(pcf_param_table, bst->name,
			      PCF_PARAM_FLAG_BUILTIN, (void *) bst,
			      pcf_conv_nbool_parameter);
    for (lst = pcf_long_table; lst->name; lst++)
	PCF_PARAM_TABLE_ENTER(pcf_param_table, lst->name,
			      PCF_PARAM_FLAG_BUILTIN, (void *) lst,
			      pcf_conv_long_parameter);

    /*
     * Register legacy parameters (used as a backwards-compatible migration
     * aid).
     */
    for (cst = pcf_legacy_str_table; cst->name; cst++)
	PCF_PARAM_TABLE_ENTER(pcf_param_table, cst->name,
			      PCF_PARAM_FLAG_LEGACY, (void *) cst,
			      pcf_conv_str_parameter);

    /*
     * Register parameters whose default value is normally initialized by
     * ad-hoc code.
     */
    pcf_adhoc_procname.defval = mystrdup(procname);
    PCF_PARAM_TABLE_ENTER(pcf_param_table, pcf_adhoc_procname.name,
			  PCF_PARAM_FLAG_BUILTIN | PCF_PARAM_FLAG_READONLY,
		      (void *) &pcf_adhoc_procname, pcf_conv_str_parameter);
    pcf_adhoc_servname.defval = mystrdup("");
    PCF_PARAM_TABLE_ENTER(pcf_param_table, pcf_adhoc_servname.name,
			  PCF_PARAM_FLAG_BUILTIN | PCF_PARAM_FLAG_READONLY,
		      (void *) &pcf_adhoc_servname, pcf_conv_str_parameter);
    pcf_adhoc_pid.defval = pid;
    PCF_PARAM_TABLE_ENTER(pcf_param_table, pcf_adhoc_pid.name,
			  PCF_PARAM_FLAG_BUILTIN | PCF_PARAM_FLAG_READONLY,
			  (void *) &pcf_adhoc_pid, pcf_conv_int_parameter);
}
