/*
**  mod_sundown.c -- Apache sundown module
**
**  Then activate it in Apache's httpd.conf file:
**
**    # httpd.conf
**    LoadModule sundown_module modules/mod_sundown.so
**    SundownStylePath      /var/www/html/style
**    SundownStyleDefault   default
**    SundownStyleExtension .html
**    SundownPageDefault    /var/www/html/README.md
**    <Location /sundown>
**      SetHandler sundown
**    </Location>
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

/* httpd */
#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "http_main.h"
#include "http_log.h"
#include "util_script.h"
#include "ap_config.h"
#include "apr_fnmatch.h"
#include "apr_strings.h"
#include "apr_hash.h"

/* apreq2 */
#include "apreq2/apreq_module_apache2.h"

/* log */
#ifdef AP_SUNDOWN_DEBUG_LOG_LEVEL
#define SUNDOWN_DEBUG_LOG_LEVEL AP_SUNDOWN_DEBUG_LOG_LEVEL
#else
#define SUNDOWN_DEBUG_LOG_LEVEL APLOG_DEBUG
#endif

#define _RERR(r, format, args...)                                           \
    ap_log_rerror(APLOG_MARK, APLOG_CRIT, 0,                                \
                  r, "[SUNDOWN] %s(%d): "format, __FILE__, __LINE__, ##args)
#define _SERR(s, format, args...)                                           \
    ap_log_error(APLOG_MARK, APLOG_CRIT, 0,                                 \
                 s, "[SUNDOWN] %s(%d): "format, __FILE__, __LINE__, ##args)
#define _PERR(p, format, args...)                                            \
    ap_log_perror(APLOG_MARK, APLOG_CRIT, 0,                                 \
                  p, "[SUNDOWN] %s(%d): "format, __FILE__, __LINE__, ##args)
#define _RDEBUG(r, format, args...)                       \
    ap_log_rerror(APLOG_MARK, SUNDOWN_DEBUG_LOG_LEVEL, 0, \
                  r, "[SUNDOWN_DEBUG] %s(%d): "format,    \
                  __FILE__, __LINE__, ##args)
#define _SDEBUG(s, format, args...)                      \
    ap_log_error(APLOG_MARK, SUNDOWN_DEBUG_LOG_LEVEL, 0, \
                 s, "[SUNDOWN_DEBUG] %s(%d): "format,    \
                 __FILE__, __LINE__, ##args)
#define _PDEBUG(p, format, args...)                       \
    ap_log_perror(APLOG_MARK, SUNDOWN_DEBUG_LOG_LEVEL, 0, \
                  p, "[SUNDOWN_DEBUG] %s(%d): "format,    \
                  __FILE__, __LINE__, ##args)

/* libcurl */
#include "curl/curl.h"

/* sundown */
#include "sundown/markdown.h"
#include "sundown/html.h"
#include "sundown/buffer.h"

#define SUNDOWN_READ_UNIT      1024
#define SUNDOWN_OUTPUT_UNIT    64
#define SUNDOWN_CURL_TIMEOUT   30
#define SUNDOWN_TITLE_DEFAULT  "Markdown Proxy"
#define SUNDOWN_CONTENT_TYPE   "text/html"
#define SUNDOWN_TAG            "<body*>"
#define SUNDOWN_STYLE_DEFAULT  "default"
#define SUNDOWN_STYLE_EXT      ".html"

typedef struct {
    char *style_path;
    char *style_default;
    char *style_ext;
    char *page_default;
} sundown_config_rec;

module AP_MODULE_DECLARE_DATA sundown_module;


static int
output_style_header(request_rec *r, apr_file_t *fp)
{
    char buf[HUGE_STRING_LEN];
    char *lower = NULL;

    while (apr_file_gets(buf, HUGE_STRING_LEN, fp) == APR_SUCCESS) {
        ap_rputs(buf, r);

        lower = apr_pstrdup(r->pool, buf);
        ap_str_tolower(lower);
        if (apr_fnmatch("*"SUNDOWN_TAG"*", lower, APR_FNM_CASE_BLIND) == 0) {
            return 1;
        }
    }

    return 0;
}

static apr_file_t *
style_header(request_rec *r, char *filename)
{
    apr_status_t rc = -1;
    apr_file_t *fp = NULL;
    char *style_filepath = NULL;
    sundown_config_rec *cfg;

    cfg = ap_get_module_config(r->per_dir_config, &sundown_module);

    if (filename == NULL && cfg->style_default != NULL) {
        filename = cfg->style_default;
    }

    if (filename != NULL) {
        if (cfg->style_path == NULL) {
            ap_add_common_vars(r);
            cfg->style_path = (char *)apr_table_get(r->subprocess_env,
                                                    "DOCUMENT_ROOT");
        }

        style_filepath = apr_psprintf(r->pool, "%s/%s%s",
                                      cfg->style_path, filename, cfg->style_ext);

        rc = apr_file_open(&fp, style_filepath,
                           APR_READ | APR_BINARY | APR_XTHREAD,
                           APR_OS_DEFAULT, r->pool);
        if (rc == APR_SUCCESS) {
            if (output_style_header(r, fp) != 1) {
                apr_file_close(fp);
                fp = NULL;
            }
        } else {
            style_filepath = apr_psprintf(r->pool, "%s/%s%s",
                                          cfg->style_path,
                                          cfg->style_default,
                                          cfg->style_ext);

            rc = apr_file_open(&fp, style_filepath,
                               APR_READ | APR_BINARY | APR_XTHREAD,
                               APR_OS_DEFAULT, r->pool);
            if (rc == APR_SUCCESS) {
                if (output_style_header(r, fp) != 1) {
                    apr_file_close(fp);
                    fp = NULL;
                }
            }
        }
    }

    if (rc != APR_SUCCESS) {
        ap_rputs("<!DOCTYPE html>\n<html>\n", r);
        ap_rputs("<head><title>"SUNDOWN_TITLE_DEFAULT"</title></head>\n", r);
        ap_rputs("<body>\n", r);
    }

    return fp;
}

static int
style_footer(request_rec *r, apr_file_t *fp) {
    char buf[HUGE_STRING_LEN];

    if (fp != NULL) {
        while (apr_file_gets(buf, HUGE_STRING_LEN, fp) == APR_SUCCESS) {
            ap_rputs(buf, r);
        }
        apr_file_close(fp);
    } else {
        ap_rputs("</body>\n</html>\n", r);
    }

    return 0;
}

static void
append_data(struct buf *ib, void *buffer, size_t size)
{
    size_t offset = 0;

    if (!ib || !buffer || size == 0) {
        return;
    }

    while (offset < size) {
        size_t bufsize = ib->asize - ib->size;
        if (size >= (bufsize + offset)) {
            memcpy(ib->data + ib->size, buffer + offset, bufsize);
            ib->size += bufsize;
            bufgrow(ib, ib->size + SUNDOWN_READ_UNIT);
            offset += bufsize;
        } else {
            bufsize = size - offset;
            if (bufsize > 0) {
                memcpy(ib->data + ib->size, buffer + offset, bufsize);
                ib->size += bufsize;
            }
            break;
        }
    }
}

static size_t
append_url_data(void *buffer, size_t size, size_t nmemb, void *user)
{
    size_t segsize = size * nmemb;
    struct buf *ib = (struct buf *)user;

    append_data(ib, buffer, segsize);

    return segsize;
}

static int
append_page_data(request_rec *r, struct buf *ib, char *filename)
{
    apr_status_t rc = -1;
    apr_file_t *fp = NULL;
    apr_size_t read;

    if (filename == NULL) {
        sundown_config_rec *cfg;
        cfg = ap_get_module_config(r->per_dir_config, &sundown_module);

        if (!cfg->page_default) {
            return HTTP_NOT_FOUND;
        }

        filename = cfg->page_default;
    } else if (strlen(filename) <= 0 ||
               memcmp(filename + strlen(filename) - 1, "/", 1) == 0) {
        return HTTP_FORBIDDEN;
    }

    rc = apr_file_open(&fp, filename,
                       APR_READ | APR_BINARY | APR_XTHREAD, APR_OS_DEFAULT,
                       r->pool);
    if (rc != APR_SUCCESS || !fp) {
        switch (errno) {
            case ENOENT:
                return HTTP_NOT_FOUND;
            case EACCES:
                return HTTP_FORBIDDEN;
            default:
                break;
        }
        return HTTP_INTERNAL_SERVER_ERROR;
    }

    do {
        rc = apr_file_read_full(fp, ib->data + ib->size, ib->asize - ib->size,
                                &read);
        if (read > 0) {
            ib->size += read;
            bufgrow(ib, ib->size + SUNDOWN_READ_UNIT);
        }
    } while (rc != APR_EOF);

    apr_file_close(fp);

    return APR_SUCCESS;
}

/* content handler */
static int
sundown_handler(request_rec *r)
{
    int ret = -1;
    apr_file_t *fp = NULL;
    char *url = NULL;
    char *style = NULL;
    char *text = NULL;
    char *raw = NULL;
    apreq_handle_t *apreq;
    apr_table_t *params;

    /* sundown: markdown */
    struct buf *ib, *ob;
    struct sd_callbacks callbacks;
    struct html_renderopt options;
    struct sd_markdown *markdown;
    unsigned int markdown_extensions = 0;

    if (strcmp(r->handler, "sundown")) {
        return DECLINED;
    }

    if (r->header_only) {
        return OK;
    }

    /* set contest type */
    r->content_type = SUNDOWN_CONTENT_TYPE;

    /* get parameter */
    apreq = apreq_handle_apache2(r);
    params = apreq_params(apreq, r->pool);
    if (params) {
        url = (char *)apreq_params_as_string(r->pool, params,
                                             "url", APREQ_JOIN_AS_IS);
        style = (char *)apreq_params_as_string(r->pool, params,
                                               "style", APREQ_JOIN_AS_IS);
#ifdef SUNDOWN_RAW_SUPPORT
        raw = apr_table_get(params, "raw");
#endif
        if (r->method_number == M_POST) {
            text = (char *)apreq_params_as_string(r->pool, params,
                                                  "markdown", APREQ_JOIN_AS_IS);
        }
    }

    /* reading everything */
    ib = bufnew(SUNDOWN_READ_UNIT);
    bufgrow(ib, SUNDOWN_READ_UNIT);

    /* page */
    append_page_data(r, ib, r->filename);

    /* text */
    if (text && strlen(text) > 0) {
        append_data(ib, text, strlen(text));
    }

    /* url */
    if (url && strlen(url) > 0) {
        CURL *curl;

        curl = curl_easy_init();
        if (!curl) {
            return HTTP_INTERNAL_SERVER_ERROR;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url);

        /* curl */
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)ib);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_url_data);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, SUNDOWN_CURL_TIMEOUT);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1);

        ret = curl_easy_perform(curl);

        curl_easy_cleanup(curl);

        /*
        if (ret != 0) {
            bufrelease(ib);
            return HTTP_INTERNAL_SERVER_ERROR;
        }
        */
    }

    /* default page */
    if (ib->size == 0) {
        ret = append_page_data(r, ib, NULL);
        if (ret != APR_SUCCESS) {
            bufrelease(ib);
            return ret;
        }
    }

    if (ib->size > 0) {
#ifdef SUNDOWN_RAW_SUPPORT
        if (raw != NULL) {
            ap_rwrite(ib->data, ib->size, r);
            bufrelease(ib);
            return OK;
        }
#endif

        /* performing markdown parsing */
        ob = bufnew(SUNDOWN_OUTPUT_UNIT);

        sdhtml_renderer(&callbacks, &options, 0);

        /* extensionss */
        markdown_extensions = 0;

#ifdef SUNDOWN_USE_FENCED_CODE
        markdown_extensions = markdown_extensions | MKDEXT_FENCED_CODE;
#endif
#ifdef SUNDOWN_USE_NO_INTRA_EMPHASIS
        markdown_extensions = markdown_extensions | MKDEXT_NO_INTRA_EMPHASIS;
#endif
#ifdef SUNDOWN_USE_AUTOLINK
        markdown_extensions = markdown_extensions | MKDEXT_AUTOLINK;
#endif
#ifdef SUNDOWN_USE_STRIKETHROUGH
        markdown_extensions = markdown_extensions | MKDEXT_STRIKETHROUGH;
#endif
#ifdef SUNDOWN_USE_LAX_HTML_BLOCKS
        markdown_extensions = markdown_extensions | MKDEXT_LAX_HTML_BLOCKS;
#endif
#ifdef SUNDOWN_USE_SPACE_HEADERS
        markdown_extensions = markdown_extensions | MKDEXT_SPACE_HEADERS;
#endif
#ifdef SUNDOWN_USE_SUPERSCRIPT
        markdown_extensions = markdown_extensions | MKDEXT_SUPERSCRIPT;
#endif
#ifdef SUNDOWN_USE_FENCED_CODE
        markdown_extensions = markdown_extensions | MKDEXT_FENCED_CODE;
#endif
#ifdef SUNDOWN_USE_TABLES
        markdown_extensions = markdown_extensions | MKDEXT_TABLES;
#endif

        markdown = sd_markdown_new(markdown_extensions, 16,
                                   &callbacks, &options);

        sd_markdown_render(ob, ib->data, ib->size, markdown);
        sd_markdown_free(markdown);

        /* output style header */
        fp = style_header(r, style);

        /* writing the result */
        ap_rwrite(ob->data, ob->size, r);

        /* cleanup */
        bufrelease(ob);
    } else {
        /* output style header */
        fp = style_header(r, style);
    }

    /* cleanup */
    bufrelease(ib);

    /* output style footer */
    style_footer(r, fp);

    return OK;
}

static void *
sundown_create_dir_config(apr_pool_t *p, char *dir)
{
    sundown_config_rec *cfg;

    cfg = apr_pcalloc(p, sizeof(sundown_config_rec));

    memset(cfg, 0, sizeof(sundown_config_rec));

    cfg->style_path = NULL;
    cfg->style_default = NULL;
    cfg->style_ext = SUNDOWN_STYLE_EXT;
    cfg->page_default = NULL;

    return (void *)cfg;
}

static const command_rec
sundown_cmds[] = {
    AP_INIT_TAKE1("SundownStylePath", ap_set_string_slot,
                  (void *)APR_OFFSETOF(sundown_config_rec, style_path),
                  OR_ALL, "sundown proxy style path"),
    AP_INIT_TAKE1("SundownStyleDefault", ap_set_string_slot,
                  (void *)APR_OFFSETOF(sundown_config_rec, style_default),
                  OR_ALL, "sundown proxy default style file name"),
    AP_INIT_TAKE1("SundownStyleExtension", ap_set_string_slot,
                  (void *)APR_OFFSETOF(sundown_config_rec, style_ext),
                  OR_ALL, "sundown proxy default style file extension"),
    AP_INIT_TAKE1("SundownPageDefault", ap_set_string_slot,
                  (void *)APR_OFFSETOF(sundown_config_rec, page_default),
                  OR_ALL, "sundown proxy default page file"),
    {NULL}
};

static void
sundown_register_hooks(apr_pool_t *p)
{
    ap_hook_handler(sundown_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA sundown_module =
{
    STANDARD20_MODULE_STUFF,
    sundown_create_dir_config, /* create per-dir    config structures */
    NULL,                      /* merge  per-dir    config structures */
    NULL,                      /* create per-server config structures */
    NULL,                      /* merge  per-server config structures */
    sundown_cmds,              /* table of config file commands       */
    sundown_register_hooks     /* register hooks                      */
};
