/*
 * $Id$
 *
 * Copyright (c) 2009 NLNet Labs. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * File Adapters.
 *
 */

#include "adapter/adapter.h"
#include "adapter/adfile.h"
#include "config.h"
#include "shared/log.h"
#include "signer/zone.h"
#include "signer/zonedata.h"
#include "util/duration.h"
#include "util/file.h"
#include "util/se_malloc.h"
#include "util/util.h"

#include <ldns/ldns.h> /* ldns_*() */
#include <stdio.h> /* rewind() */

static const char* adapter_str = "adapter";

static int adfile_read_file(FILE* fd, struct zone_struct* zone, int include,
    int recover);


/**
 * Check for white space.
 *
 */
static int
line_contains_space_only(char* line, int line_len)
{
    int i;
    for (i = 0; i < line_len; i++) {
        if (!isspace((int)line[i])) {
            return 0;
        }
    }
    return 1;
}


/*
 * Trim trailing whitespace.
 *
 */
static void
adfile_rtrim(char* line, int* line_len)
{
    int i = strlen(line), nl = 0;
    int trimmed = 0;

    while (i>0) {
        --i;
        if (line[i] == '\n') {
            nl = 1;
        }
        if (line[i] == ' ' || line[i] == '\t' || line[i] == '\n') {
            line[i] = '\0';
            trimmed++;
        } else {
            break;
        }
    }
    if (nl) {
        line[++i] = '\n';
    }
    *line_len -= trimmed;
    return;
}


/**
 * Read one line from zone file.
 *
 */
static int
adfile_read_line(FILE* fd, char* line, unsigned int* l)
{
    int i = 0;
    int li = 0;
    int in_string = 0;
    int depth = 0;
    int comments = 0;
    char c = 0;
    char lc = 0;

    for (i = 0; i < SE_ADFILE_MAXLINE; i++) {
        c = (char) se_fgetc(fd, l);
        if (comments) {
            while (c != EOF && c != '\n') {
                c = (char) se_fgetc(fd, l);
            }
        }

        if (c == EOF) {
            if (depth != 0) {
                ods_log_error("[%s] read line: bracket mismatch discovered at "
                    "line %i, missing ')'", adapter_str, l&&*l?*l:0);
            }
            if (li > 0) {
                line[li] = '\0';
                return li;
            } else {
                return -1;
            }
        } else if (c == '"' && lc != '\\') {
            in_string = 1 - in_string; /* swap status */
            line[li] = c;
            li++;
        } else if (c == '(') {
            if (in_string) {
                line[li] = c;
                li++;
            } else if (lc != '\\') {
                depth++;
                line[li] = ' ';
                li++;
            } else {
                line[li] = c;
                li++;
            }
        } else if (c == ')') {
            if (in_string) {
                line[li] = c;
                li++;
            } else if (lc != '\\') {
                if (depth < 1) {
                    ods_log_error("[%s] read line: bracket mismatch discovered at "
                        "line %i, missing '('", adapter_str, l&&*l?*l:0);
                    line[li] = '\0';
                    return li;
                }
                depth--;
                line[li] = ' ';
                li++;
            } else {
                line[li] = c;
                li++;
            }
        } else if (c == ';') {
            if (in_string) {
                line[li] = c;
                li++;
            } else if (lc != '\\') {
                comments = 1;
            } else {
                line[li] = c;
                li++;
            }
        } else if (c == '\n' && lc != '\\') {
            comments = 0;
            /* if no depth issue, we are done */
            if (depth == 0) {
                break;
            }
            line[li] = ' ';
            li++;
        } else {
            line[li] = c;
            li++;
        }
        /* continue with line */
        lc = c;
    }

    /* done */
    if (depth != 0) {
        ods_log_error("[%s]read line: bracket mismatch discovered at line %i, "
            "missing ')'", adapter_str, l&&*l?*l:0);
        return li;
    }
    line[li] = '\0';
    return li;
}


/**
 * Lookup SOA RR.
 *
 */
static ldns_rr*
adfile_lookup_soa_rr(FILE* fd)
{
    ldns_rr *cur_rr = NULL;
    char line[SE_ADFILE_MAXLINE];
    ldns_status status = LDNS_STATUS_OK;
    int line_len = 0;
    unsigned int l = 0;

    while (line_len >= 0) {
        line_len = adfile_read_line(fd, (char*) line, &l);
        adfile_rtrim(line, &line_len);

        if (line_len > 0) {
            if (line[0] != ';') {
                status = ldns_rr_new_frm_str(&cur_rr, line, 0, NULL, NULL);
                if (status == LDNS_STATUS_OK) {
                    if (ldns_rr_get_type(cur_rr) == LDNS_RR_TYPE_SOA) {
                        return cur_rr;
                    } else {
                        ldns_rr_free(cur_rr);
                        cur_rr = NULL;
                    }
                }
            }
        }
    }
    return NULL;
}


/**
 * Read the next RR from zone file.
 *
 */
static ldns_rr*
adfile_read_rr(FILE* fd, zone_type* adzone, char* line, ldns_rdf** orig,
    ldns_rdf** prev, uint32_t* ttl, ldns_status* status, unsigned int* l,
    int recover)
{
    ldns_rr* rr = NULL;
    ldns_rdf* tmp = NULL;
    FILE* fd_include = NULL;
    int len = 0, error = 0;
    uint32_t new_ttl = 0;
    const char *endptr;  /* unused */
    int offset = 0;

adfile_read_line:
    if (ttl) {
        new_ttl = *ttl;
    }

    len = adfile_read_line(fd, line, l);
    adfile_rtrim(line, &len);

    if (len >= 0) {
        switch (line[0]) {
            /* directive */
            case '$':
                if (strncmp(line, "$ORIGIN", 7) == 0 && isspace(line[7])) {
                    /* copy from ldns */
                    if (*orig) {
                        ldns_rdf_deep_free(*orig);
                        *orig = NULL;
                    }
                    offset = 8;
                    while (isspace(line[offset])) {
                        offset++;
                    }
                    tmp = ldns_rdf_new_frm_str(LDNS_RDF_TYPE_DNAME, line + offset);
                    if (!tmp) {
                        /* could not parse what next to $ORIGIN */
                        *status = LDNS_STATUS_SYNTAX_DNAME_ERR;
                        return NULL;
                    }
                    *orig = tmp;
                    /* end copy from ldns */
                    goto adfile_read_line; /* perhaps next line is rr */
                    break;
                } else if (strncmp(line, "$TTL", 4) == 0 &&
                    isspace(line[4])) {
                    /* override default ttl */
                    offset = 5;
                    while (isspace(line[offset])) {
                        offset++;
                    }
                    if (ttl) {
                        *ttl = ldns_str2period(line + offset, &endptr);
                        new_ttl = *ttl;
                    }
                    goto adfile_read_line; /* perhaps next line is rr */
                    break;
                } else if (strncmp(line, "$INCLUDE", 8) == 0 &&
                    isspace(line[8])) {
                    /* dive into this file */
                    offset = 9;
                    while (isspace(line[offset])) {
                        offset++;
                    }
                    fd_include = se_fopen(line + offset, NULL, "r");
                    if (fd_include) {
                        error = adfile_read_file(fd_include, adzone, 1,
                            recover);
                        se_fclose(fd_include);
                    } else {
                        ods_log_error("[%s] unable to open include file %s",
                            adapter_str, (line+offset));
                        *status = LDNS_STATUS_SYNTAX_ERR;
                        return NULL;
                    }
                    if (error) {
                        *status = LDNS_STATUS_ERR;
                        ods_log_error("[%s] error in include file %s",
                            adapter_str, (line+offset));
                        return NULL;
                    }
                    /* restore current ttl */
                    *ttl = new_ttl;
                    goto adfile_read_line; /* perhaps next line is rr */
                    break;
                }
                goto adfile_read_rr; /* this can be an owner name */
                break;
            /* comments, empty lines */
            case ';':
            case '\n':
                goto adfile_read_line; /* perhaps next line is rr */
                break;
            /* let's hope its a RR */
            default:
adfile_read_rr:
                if (line_contains_space_only(line, len)) {
                    goto adfile_read_line; /* perhaps next line is rr */
                    break;
                }

                *status = ldns_rr_new_frm_str(&rr, line, new_ttl, *orig, prev);
                if (*status == LDNS_STATUS_OK) {
                    ldns_rr2canonical(rr); /* TODO: canonicalize or not? */
                    return rr;
                } else if (*status == LDNS_STATUS_SYNTAX_EMPTY) {
                    if (rr) {
                        ldns_rr_free(rr);
                        rr = NULL;
                    }
                    *status = LDNS_STATUS_OK;
                    goto adfile_read_line; /* perhaps next line is rr */
                    break;
                } else {
                    ods_log_error("[%s] error parsing RR at line %i (%s): %s",
                        adapter_str, l&&*l?*l:0,
                        ldns_get_errorstr_by_id(*status), line);
                    while (len >= 0) {
                        len = adfile_read_line(fd, line, l);
                    }
                    if (rr) {
                        ldns_rr_free(rr);
                        rr = NULL;
                    }
                    return NULL;
                }
                break;
        }
    }

    /* -1, EOF */
    *status = LDNS_STATUS_OK;
    return NULL;
}


/**
 * Read zone file.
 *
 */
static int
adfile_read_file(FILE* fd, struct zone_struct* zone, int include, int recover)
{
    int result = 0;
    uint32_t soa_min = 0;
    zone_type* adzone = zone;
    ldns_rr* rr = NULL;
    ldns_rdf* prev = NULL;
    ldns_rdf* orig = NULL;
    ldns_status status = LDNS_STATUS_OK;
    char line[SE_ADFILE_MAXLINE];
    unsigned int line_update_interval = 100000;
    unsigned int line_update = line_update_interval;
    unsigned int l = 0;

    ods_log_assert(fd);
    ods_log_assert(zone);

    if (!zone->signconf) {
        ods_log_error("[%s] read file failed, no signconf", adapter_str);
        return 1;
    }
    ods_log_assert(zone->signconf);

    if (!include) {
        rr = adfile_lookup_soa_rr(fd);
        /* default TTL: taking the SOA MINIMUM is in conflict with RFC2308 */
        if (adzone->signconf->soa_min) {
            soa_min = (uint32_t) duration2time(adzone->signconf->soa_min);
        } else if (rr) {
            soa_min = ldns_rdf2native_int32(ldns_rr_rdf(rr,
                SE_SOA_RDATA_MINIMUM));
        }
        adzone->zonedata->default_ttl = soa_min;
        /* serial */
        if (rr) {
            adzone->zonedata->inbound_serial =
                ldns_rdf2native_int32(ldns_rr_rdf(rr, SE_SOA_RDATA_SERIAL));
            ldns_rr_free(rr);
        }
        rewind(fd);

        if (se_strcmp(adzone->signconf->soa_serial, "keep") == 0) {
            if (adzone->zonedata->inbound_serial <=
                adzone->zonedata->outbound_serial) {
                ods_log_error("[%s] read file failed, zone %s SOA SERIAL is "
                    " set to keep, but serial %u in input zone is lower than "
                    " current serial %u", adapter_str, zone->name,
                    zone->zonedata->inbound_serial,
                    zone->zonedata->outbound_serial);
                return 1;
            }
        }
    }

    /* $ORIGIN <zone name> */
    orig = ldns_rdf_clone(adzone->dname);

    ods_log_debug("[%s] start reading them RRs...", adapter_str);

    /* read records */
    while ((rr = adfile_read_rr(fd, adzone, line, &orig, &prev,
        &(adzone->zonedata->default_ttl), &status, &l, recover)) != NULL) {

        if (status != LDNS_STATUS_OK) {
            ods_log_error("[%s] error reading RR at line %i (%s): %s",
                adapter_str, l, ldns_get_errorstr_by_id(status), line);
            result = 1;
            break;
        }

        if (l > line_update) {
            ods_log_debug("[%s] ...at line %i: %s", adapter_str, l, line);
            line_update += line_update_interval;
        }

        /* filter out DNSSEC RRs (except DNSKEY) from the Input File Adapter */
        if (util_is_dnssec_rr(rr)) {
            ldns_rr_free(rr);
            rr = NULL;
            continue;
        }

        /* add to the zonedata */
        result = zone_add_rr(adzone, rr, recover);
        if (result != 0) {
            ods_log_error("[%s] error adding RR at line %i: %s",
                adapter_str, l, line);
            break;
        }
        adzone->stats->sort_count += 1;
    }

    /* and done */
    if (orig) {
        ldns_rdf_deep_free(orig);
        orig = NULL;
    }
    if (prev) {
        ldns_rdf_deep_free(prev);
        prev = NULL;
    }

    if (!result && status != LDNS_STATUS_OK) {
        ods_log_error("[%s] error reading RR at line %i (%s): %s",
            adapter_str, l, ldns_get_errorstr_by_id(status), line);
        result = 1;
    }

    /* reset the default ttl (directives only affect the zone file) */
    adzone->zonedata->default_ttl = soa_min;

    return result;
}


/**
 * Read input file adapter.
 *
 */
int
adfile_read(struct zone_struct* zone, const char* filename, int recover)
{
    FILE* fd = NULL;
    zone_type* adzone = zone;
    int error = 0;

    if (!adzone || !adzone->name) {
        ods_log_error("[%s] unable to read file: no zone (or no "
            "name given)", adapter_str);
        return 1;
    }
    ods_log_assert(adzone);
    ods_log_assert(adzone->name);

    if (!filename) {
        ods_log_error("[%s] unable to read file: no filename given",
            adapter_str);
        return 1;
    }
    ods_log_assert(filename);

    ods_log_debug("[%s] read zone %s from file %s", adapter_str,
        adzone->name, filename);

    /* read the zonefile */
    fd = se_fopen(filename, NULL, "r");
    if (fd) {
        if (recover) {
            error = adfile_read_file(fd, adzone, 1, 1);
        } else {
            error = adfile_read_file(fd, adzone, 0, 0);
        }
        se_fclose(fd);
    } else {
        error = 1;
    }
    if (error) {
        ods_log_error("[%s] read zone %s from file %s failed",
            adapter_str, adzone->name?adzone->name:"(null)",
            filename?filename:"(null)");
        return error;
    }

    if (!recover) {
        /* remove current rrs */
        error = zonedata_del_rrs(adzone->zonedata);
        if (error) {
            ods_log_error("[%s] error removing current RRs in zone %s",
                adapter_str, adzone->name?adzone->name:"(null)");
        }
    }
    return error;
}


/**
 * Write zone file.
 *
 */
int
adfile_write(struct zone_struct* zone, const char* filename)
{
    FILE* fd = NULL;
    zone_type* adzone = (zone_type*) zone;

    if (!adzone || !adzone->name) {
        ods_log_error("[%s] unable to write file: no zone (or no "
            "name given)", adapter_str);
        return 1;
    }
    ods_log_assert(adzone);
    ods_log_assert(adzone->name);

    if (!filename) {
        ods_log_assert(adzone->outbound_adapter->filename);
        ods_log_debug("[%s] write zone %s to file %s", adapter_str,
            adzone->name, adzone->outbound_adapter->filename);
        fd = se_fopen(adzone->outbound_adapter->filename, NULL, "w");
    } else {
        ods_log_debug("[%s] write zone %s to file %s", adapter_str,
            adzone->name, filename);
        fd = se_fopen(filename, NULL, "w");
    }

    if (fd) {
        zone_print(fd, adzone);
        se_fclose(fd);
    }
    return 0;
}