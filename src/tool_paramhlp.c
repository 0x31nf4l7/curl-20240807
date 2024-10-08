/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 * SPDX-License-Identifier: curl
 *
 ***************************************************************************/
#include "tool_setup.h"

#include "strcase.h"

#include "curlx.h"

#include "tool_cfgable.h"
#include "tool_getparam.h"
#include "tool_getpass.h"
#include "tool_msgs.h"
#include "tool_paramhlp.h"
#include "tool_libinfo.h"
#include "tool_util.h"
#include "tool_version.h"
#include "dynbuf.h"

#include "memdebug.h" /* keep this as LAST include */

/* This function compensates the shortcomings of fseek in windows for
text-mode filestreams: we assume that a user considers a line break as
one character '\n' and not "\r\n" */
int textmode_fseek(FILE *stream, curl_off_t offset, int whence)
{
  #if defined(HAVE__FSEEKI64)
  #define FSEEK(S, O, W) _fseeki64(S, (__int64)O, W)
  #elif defined(HAVE_FSEEKO) && defined(HAVE_DECL_FSEEKO)
  #define FSEEK(S, O, W) fseeko(S, (off_t)O, W)
  #else
  #define FSEEK(S, O, W) ((O > LONG_MAX) ? -1 : fseek(S, (long)O, W))
  #endif
  curl_off_t pos, newpos = 0;

  if(whence == SEEK_END && FSEEK(stream, 0, SEEK_END))
    return -1;

  pos = (curl_off_t)ftell(stream);
  if(pos == -1)
    return -1;

  if(FSEEK(stream, 0, SEEK_SET))
    return -1;

  if(whence == SEEK_SET)
    newpos = offset;
  else
    newpos = offset + pos;
  pos = 0;
  while(pos < newpos && fgetc(stream) != EOF)
    pos++;

  return (pos == newpos) ? 0 : -1;
  #undef FSEEK
}

/* filename_extract_limits() examines the entered filename. It starts
at the end of the string and works its way to the pointer filename. If
the string ends with "!x-y" or "!x-" or "!-y", then x and/or y is stored
in the addresses of start and end.
(1) in case "!x-y", x is stored in the memory of start and y in the
    memory of end. The return value of filename_extract_limits() here is
    FILELIMIT_START | FILELIMIT_END.
(2) in case "-y", y is stored in the memory of end and the
    filename_extract_limits() function returns FILELIMIT_END.
(3) in case "x-", x is stored in the memory of start and the
    filename_extract_limits() function returns FILELIMIT_START.
(4) if x or y are not numbers, 0 is returned and the memory of start and end
    remains unchanged. */
int filename_extract_limits(char *filename, curl_off_t *start,
                            curl_off_t *end)
{
  char *ptr_range;
  int flags_ret = 0;
  bool have_minus = FALSE;

  ptr_range = filename + strlen(filename) - 1;
  while(ptr_range > filename) {
    if(!strchr("0123456789-!", *ptr_range))
      return 0;

    if(*ptr_range == '-') {
      have_minus = TRUE;
    }
    if(*ptr_range == '!') {
      if(*(ptr_range + 1) != '-') {
        flags_ret |= FILELIMIT_START;
        if(curlx_strtoofft(ptr_range + 1, NULL, 10, start) != CURL_OFFT_OK)
          return 0;
      }
      if(have_minus && flags_ret)
        *ptr_range = '\0';
      break;
    }
    else if(*ptr_range == '-' && strchr(ptr_range + 1, '-'))
      return 0;

    else if(*ptr_range == '-' && *(ptr_range + 1) != '\0') {
      flags_ret = FILELIMIT_END;
      if(curlx_strtoofft(ptr_range + 1, NULL, 10, end) != CURL_OFFT_OK)
        return 0;
    }
    ptr_range--;
  }
  return flags_ret;
}

struct getout *new_getout(struct OperationConfig *config)
{
  struct getout *node = calloc(1, sizeof(struct getout));
  struct getout *last = config->url_last;
  if(node) {
    static int outnum = 0;

    /* append this new node last in the list */
    if(last)
      last->next = node;
    else
      config->url_list = node; /* first node */

    /* move the last pointer */
    config->url_last = node;

    node->flags = config->default_node_flags;
    node->num = outnum++;
  }
  return node;
}

#define ISCRLF(x) (((x) == '\r') || ((x) == '\n') || ((x) == '\0'))

/* memcrlf() has two modes. Both operate on a given memory area with
   a specified size.

   countcrlf FALSE - return number of bytes from the start that DO NOT include
   any CR or LF or NULL

   countcrlf TRUE - return number of bytes from the start that are ONLY CR or
   LF or NULL.

*/
static size_t memcrlf(char *orig,
                      bool countcrlf, /* TRUE if we count CRLF, FALSE
                                         if we count non-CRLF */
                      size_t max)
{
  char *ptr;
  size_t total = max;
  for(ptr = orig; max; max--, ptr++) {
    bool crlf = ISCRLF(*ptr);
    if(countcrlf ^ crlf)
      return ptr - orig;
  }
  return total; /* no delimiter found */
}

#define MAX_FILE2STRING MAX_FILE2MEMORY

ParameterError file2string(char **bufp, FILE *file, int filelimit,
                           curl_off_t start, curl_off_t end)
{
  struct curlx_dynbuf dyn;
  curl_off_t rangelen = 0;
  int rangelen_set = 0;

  if(filelimit == FILELIMIT_START && textmode_fseek(file, start, SEEK_SET)) {
    return PARAM_FSEEK_ERROR;
  }
  if(filelimit == FILELIMIT_END && textmode_fseek(file, -end, SEEK_END)) {
    return PARAM_FSEEK_ERROR;
  }
  if(filelimit == (FILELIMIT_START | FILELIMIT_END)) {
    if(textmode_fseek(file, start, SEEK_SET)) {
      return PARAM_FSEEK_ERROR;
    }
    rangelen = end - start + 1;
    rangelen_set = 1;
  }
  curlx_dyn_init(&dyn, MAX_FILE2STRING);
  if(file) {
    do {
      char buffer[4096];
      char *ptr;
      size_t elt_cnt;
      size_t nread;

      if(rangelen_set && (rangelen < (curl_off_t)sizeof(buffer))) {
        elt_cnt = (size_t)rangelen;
      }
      else {
        elt_cnt = sizeof(buffer);
      }
      nread = fread(buffer, 1, elt_cnt, file);
      if(ferror(file)) {
        curlx_dyn_free(&dyn);
        *bufp = NULL;
        return PARAM_READ_ERROR;
      }
      rangelen = rangelen - nread;
      ptr = buffer;
      while(nread) {
        size_t nlen = memcrlf(ptr, FALSE, nread);
        if(curlx_dyn_addn(&dyn, ptr, nlen))
          return PARAM_NO_MEM;
        nread -= nlen;

        if(nread) {
          ptr += nlen;
          nlen = memcrlf(ptr, TRUE, nread);
          ptr += nlen;
          nread -= nlen;
        }
      }
      if(rangelen_set && rangelen <= 0)
        break;
    } while(!feof(file));
  }
  *bufp = curlx_dyn_ptr(&dyn);
  return PARAM_OK;
}

ParameterError file2memory(char **bufp, size_t *size, FILE *file,
                           int filelimit, curl_off_t start, curl_off_t end)
{
  #if defined(HAVE__FSEEKI64)
  #define FSEEK(S, O, W) _fseeki64(S, (__int64)O, W)
  #elif defined(HAVE_FSEEKO) && defined(HAVE_DECL_FSEEKO)
  #define FSEEK(S, O, W) fseeko(S, (off_t)O, W)
  #else
  #define FSEEK(S, O, W) ((O > LONG_MAX) ? -1 : fseek(S, (long)O, W))
  #endif

  if(file) {
    size_t nread;
    struct curlx_dynbuf dyn;
    curl_off_t rangelen = 0;
    int rangelen_set = 0;

    if(filelimit == FILELIMIT_START && FSEEK(file, start, SEEK_SET)) {
      return PARAM_FSEEK_ERROR;
    }
    if(filelimit == FILELIMIT_END && FSEEK(file, -end, SEEK_END)) {
      return PARAM_FSEEK_ERROR;
    }
    if(filelimit == (FILELIMIT_START | FILELIMIT_END)) {
      if(FSEEK(file, start, SEEK_SET)) {
        return PARAM_FSEEK_ERROR;
      }
      rangelen = end - start + 1;
      rangelen_set = 1;
    }
    /* The size needs to fit in an int later */
    curlx_dyn_init(&dyn, MAX_FILE2MEMORY);
    do {
      char buffer[4096];
      curl_off_t elt_cnt;

      if(rangelen_set && (rangelen < (curl_off_t)sizeof(buffer)))
        elt_cnt = rangelen;
      else
        elt_cnt = sizeof(buffer);

      nread = fread(buffer, 1, (size_t)elt_cnt, file);
      rangelen = rangelen - nread;
      if(ferror(file)) {
        curlx_dyn_free(&dyn);
        *size = 0;
        *bufp = NULL;
        return PARAM_READ_ERROR;
      }
      if(nread)
        if(curlx_dyn_addn(&dyn, buffer, nread))
          return PARAM_NO_MEM;

      if(rangelen_set && rangelen <= 0)
        break;

    } while(!feof(file));
    *size = curlx_dyn_len(&dyn);
    *bufp = curlx_dyn_ptr(&dyn);
  }
  else {
    *size = 0;
    *bufp = NULL;
  }
  return PARAM_OK;
  #undef FSEEK
}

/*
 * Parse the string and write the long in the given address. Return PARAM_OK
 * on success, otherwise a parameter specific error enum.
 *
 * Since this function gets called with the 'nextarg' pointer from within the
 * getparameter a lot, we must check it for NULL before accessing the str
 * data.
 */
static ParameterError getnum(long *val, const char *str, int base)
{
  if(str) {
    char *endptr = NULL;
    long num;
    if(!str[0])
      return PARAM_BLANK_STRING;
    errno = 0;
    num = strtol(str, &endptr, base);
    if(errno == ERANGE)
      return PARAM_NUMBER_TOO_LARGE;
    if((endptr != str) && (*endptr == '\0')) {
      *val = num;
      return PARAM_OK;  /* Ok */
    }
  }
  return PARAM_BAD_NUMERIC; /* badness */
}

ParameterError str2num(long *val, const char *str)
{
  return getnum(val, str, 10);
}

ParameterError oct2nummax(long *val, const char *str, long max)
{
  ParameterError result = getnum(val, str, 8);
  if(result != PARAM_OK)
    return result;
  else if(*val > max)
    return PARAM_NUMBER_TOO_LARGE;
  else if(*val < 0)
    return PARAM_NEGATIVE_NUMERIC;

  return PARAM_OK;
}

/*
 * Parse the string and write the long in the given address. Return PARAM_OK
 * on success, otherwise a parameter error enum. ONLY ACCEPTS POSITIVE NUMBERS!
 *
 * Since this function gets called with the 'nextarg' pointer from within the
 * getparameter a lot, we must check it for NULL before accessing the str
 * data.
 */

ParameterError str2unum(long *val, const char *str)
{
  ParameterError result = getnum(val, str, 10);
  if(result != PARAM_OK)
    return result;
  if(*val < 0)
    return PARAM_NEGATIVE_NUMERIC;

  return PARAM_OK;
}

/*
 * Parse the string and write the long in the given address if it is below the
 * maximum allowed value. Return PARAM_OK on success, otherwise a parameter
 * error enum. ONLY ACCEPTS POSITIVE NUMBERS!
 *
 * Since this function gets called with the 'nextarg' pointer from within the
 * getparameter a lot, we must check it for NULL before accessing the str
 * data.
 */

ParameterError str2unummax(long *val, const char *str, long max)
{
  ParameterError result = str2unum(val, str);
  if(result != PARAM_OK)
    return result;
  if(*val > max)
    return PARAM_NUMBER_TOO_LARGE;

  return PARAM_OK;
}


/*
 * Parse the string and write the double in the given address. Return PARAM_OK
 * on success, otherwise a parameter specific error enum.
 *
 * The 'max' argument is the maximum value allowed, as the numbers are often
 * multiplied when later used.
 *
 * Since this function gets called with the 'nextarg' pointer from within the
 * getparameter a lot, we must check it for NULL before accessing the str
 * data.
 */

static ParameterError str2double(double *val, const char *str, double max)
{
  if(str) {
    char *endptr;
    double num;
    errno = 0;
    num = strtod(str, &endptr);
    if(errno == ERANGE)
      return PARAM_NUMBER_TOO_LARGE;
    if(num > max) {
      /* too large */
      return PARAM_NUMBER_TOO_LARGE;
    }
    if((endptr != str) && (endptr == str + strlen(str))) {
      *val = num;
      return PARAM_OK;  /* Ok */
    }
  }
  return PARAM_BAD_NUMERIC; /* badness */
}

/*
 * Parse the string as seconds with decimals, and write the number of
 * milliseconds that corresponds in the given address. Return PARAM_OK on
 * success, otherwise a parameter error enum. ONLY ACCEPTS POSITIVE NUMBERS!
 *
 * The 'max' argument is the maximum value allowed, as the numbers are often
 * multiplied when later used.
 *
 * Since this function gets called with the 'nextarg' pointer from within the
 * getparameter a lot, we must check it for NULL before accessing the str
 * data.
 */

ParameterError secs2ms(long *valp, const char *str)
{
  double value;
  ParameterError result = str2double(&value, str, (double)LONG_MAX/1000);
  if(result != PARAM_OK)
    return result;
  if(value < 0)
    return PARAM_NEGATIVE_NUMERIC;

  *valp = (long)(value*1000);
  return PARAM_OK;
}

/*
 * Implement protocol sets in null-terminated array of protocol name pointers.
 */

/* Return index of prototype token in set, card(set) if not found.
   Can be called with proto == NULL to get card(set). */
static size_t protoset_index(const char * const *protoset, const char *proto)
{
  const char * const *p = protoset;

  DEBUGASSERT(proto == proto_token(proto));     /* Ensure it is tokenized. */

  for(; *p; p++)
    if(proto == *p)
      break;
  return p - protoset;
}

/* Include protocol token in set. */
static void protoset_set(const char **protoset, const char *proto)
{
  if(proto) {
    size_t n = protoset_index(protoset, proto);

    if(!protoset[n]) {
      DEBUGASSERT(n < proto_count);
      protoset[n] = proto;
      protoset[n + 1] = NULL;
    }
  }
}

/* Exclude protocol token from set. */
static void protoset_clear(const char **protoset, const char *proto)
{
  if(proto) {
    size_t n = protoset_index(protoset, proto);

    if(protoset[n]) {
      size_t m = protoset_index(protoset, NULL) - 1;

      protoset[n] = protoset[m];
      protoset[m] = NULL;
    }
  }
}

/*
 * Parse the string and provide an allocated libcurl compatible protocol
 * string output. Return non-zero on failure, zero on success.
 *
 * The string is a list of protocols
 *
 * Since this function gets called with the 'nextarg' pointer from within the
 * getparameter a lot, we must check it for NULL before accessing the str
 * data.
 */

#define MAX_PROTOSTRING (64*11) /* Enough room for 64 10-chars proto names. */

ParameterError proto2num(struct OperationConfig *config,
                         const char * const *val, char **ostr, const char *str)
{
  char *buffer;
  const char *sep = ",";
  char *token;
  const char **protoset;
  struct curlx_dynbuf obuf;
  size_t proto;
  CURLcode result;

  curlx_dyn_init(&obuf, MAX_PROTOSTRING);

  if(!str)
    return PARAM_OPTION_AMBIGUOUS;

  buffer = strdup(str); /* because strtok corrupts it */
  if(!buffer)
    return PARAM_NO_MEM;

  protoset = malloc((proto_count + 1) * sizeof(*protoset));
  if(!protoset) {
    free(buffer);
    return PARAM_NO_MEM;
  }

  /* Preset protocol set with default values. */
  protoset[0] = NULL;
  for(; *val; val++) {
    const char *p = proto_token(*val);

    if(p)
      protoset_set(protoset, p);
  }

  /* Allow strtok() here since this is not used threaded */
  /* !checksrc! disable BANNEDFUNC 2 */
  for(token = strtok(buffer, sep);
      token;
      token = strtok(NULL, sep)) {
    enum e_action { allow, deny, set } action = allow;

    /* Process token modifiers */
    while(!ISALNUM(*token)) { /* may be NULL if token is all modifiers */
      switch(*token++) {
      case '=':
        action = set;
        break;
      case '-':
        action = deny;
        break;
      case '+':
        action = allow;
        break;
      default: /* Includes case of terminating NULL */
        free(buffer);
        free((char *) protoset);
        return PARAM_BAD_USE;
      }
    }

    if(curl_strequal(token, "all")) {
      switch(action) {
      case deny:
        protoset[0] = NULL;
        break;
      case allow:
      case set:
        memcpy((char *) protoset,
               built_in_protos, (proto_count + 1) * sizeof(*protoset));
        break;
      }
    }
    else {
      const char *p = proto_token(token);

      if(p)
        switch(action) {
        case deny:
          protoset_clear(protoset, p);
          break;
        case set:
          protoset[0] = NULL;
          FALLTHROUGH();
        case allow:
          protoset_set(protoset, p);
          break;
        }
      else { /* unknown protocol */
        /* If they have specified only this protocol, we say treat it as
           if no protocols are allowed */
        if(action == set)
          protoset[0] = NULL;
        warnf(config->global, "unrecognized protocol '%s'", token);
      }
    }
  }
  free(buffer);

  /* We need the protocols in alphabetic order for CI tests requirements. */
  qsort((char *) protoset, protoset_index(protoset, NULL), sizeof(*protoset),
        struplocompare4sort);

  result = curlx_dyn_addn(&obuf, "", 0);
  for(proto = 0; protoset[proto] && !result; proto++)
    result = curlx_dyn_addf(&obuf, "%s,", protoset[proto]);
  free((char *) protoset);
  curlx_dyn_setlen(&obuf, curlx_dyn_len(&obuf) - 1);
  free(*ostr);
  *ostr = curlx_dyn_ptr(&obuf);

  return *ostr ? PARAM_OK : PARAM_NO_MEM;
}

/**
 * Check if the given string is a protocol supported by libcurl
 *
 * @param str  the protocol name
 * @return PARAM_OK  protocol supported
 * @return PARAM_LIBCURL_UNSUPPORTED_PROTOCOL  protocol not supported
 * @return PARAM_REQUIRES_PARAMETER   missing parameter
 */
ParameterError check_protocol(const char *str)
{
  if(!str)
    return PARAM_REQUIRES_PARAMETER;

  if(proto_token(str))
    return PARAM_OK;
  return PARAM_LIBCURL_UNSUPPORTED_PROTOCOL;
}

/**
 * Parses the given string looking for an offset (which may be a
 * larger-than-integer value). The offset CANNOT be negative!
 *
 * @param val  the offset to populate
 * @param str  the buffer containing the offset
 * @return PARAM_OK if successful, a parameter specific error enum if failure.
 */
ParameterError str2offset(curl_off_t *val, const char *str)
{
  char *endptr;
  if(str[0] == '-')
    /* offsets are not negative, this indicates weird input */
    return PARAM_NEGATIVE_NUMERIC;

#if(SIZEOF_CURL_OFF_T > SIZEOF_LONG)
  {
    CURLofft offt = curlx_strtoofft(str, &endptr, 10, val);
    if(CURL_OFFT_FLOW == offt)
      return PARAM_NUMBER_TOO_LARGE;
    else if(CURL_OFFT_INVAL == offt)
      return PARAM_BAD_NUMERIC;
  }
#else
  errno = 0;
  *val = strtol(str, &endptr, 0);
  if((*val == LONG_MIN || *val == LONG_MAX) && errno == ERANGE)
    return PARAM_NUMBER_TOO_LARGE;
#endif
  if((endptr != str) && (endptr == str + strlen(str)))
    return PARAM_OK;

  return PARAM_BAD_NUMERIC;
}

#define MAX_USERPWDLENGTH (100*1024)
static CURLcode checkpasswd(const char *kind, /* for what purpose */
                            const size_t i,   /* operation index */
                            const bool last,  /* TRUE if last operation */
                            char **userpwd)   /* pointer to allocated string */
{
  char *psep;
  char *osep;

  if(!*userpwd)
    return CURLE_OK;

  /* Attempt to find the password separator */
  psep = strchr(*userpwd, ':');

  /* Attempt to find the options separator */
  osep = strchr(*userpwd, ';');

  if(!psep && **userpwd != ';') {
    /* no password present, prompt for one */
    char passwd[2048] = "";
    char prompt[256];
    struct curlx_dynbuf dyn;

    curlx_dyn_init(&dyn, MAX_USERPWDLENGTH);
    if(osep)
      *osep = '\0';

    /* build a nice-looking prompt */
    if(!i && last)
      msnprintf(prompt, sizeof(prompt),
                "Enter %s password for user '%s':",
                kind, *userpwd);
    else
      msnprintf(prompt, sizeof(prompt),
                "Enter %s password for user '%s' on URL #%zu:",
                kind, *userpwd, i + 1);

    /* get password */
    getpass_r(prompt, passwd, sizeof(passwd));
    if(osep)
      *osep = ';';

    if(curlx_dyn_addf(&dyn, "%s:%s", *userpwd, passwd))
      return CURLE_OUT_OF_MEMORY;

    /* return the new string */
    free(*userpwd);
    *userpwd = curlx_dyn_ptr(&dyn);
  }

  return CURLE_OK;
}

ParameterError add2list(struct curl_slist **list, const char *ptr)
{
  struct curl_slist *newlist = curl_slist_append(*list, ptr);
  if(newlist)
    *list = newlist;
  else
    return PARAM_NO_MEM;

  return PARAM_OK;
}

int ftpfilemethod(struct OperationConfig *config, const char *str)
{
  if(curl_strequal("singlecwd", str))
    return CURLFTPMETHOD_SINGLECWD;
  if(curl_strequal("nocwd", str))
    return CURLFTPMETHOD_NOCWD;
  if(curl_strequal("multicwd", str))
    return CURLFTPMETHOD_MULTICWD;

  warnf(config->global, "unrecognized ftp file method '%s', using default",
        str);

  return CURLFTPMETHOD_MULTICWD;
}

int ftpcccmethod(struct OperationConfig *config, const char *str)
{
  if(curl_strequal("passive", str))
    return CURLFTPSSL_CCC_PASSIVE;
  if(curl_strequal("active", str))
    return CURLFTPSSL_CCC_ACTIVE;

  warnf(config->global, "unrecognized ftp CCC method '%s', using default",
        str);

  return CURLFTPSSL_CCC_PASSIVE;
}

long delegation(struct OperationConfig *config, const char *str)
{
  if(curl_strequal("none", str))
    return CURLGSSAPI_DELEGATION_NONE;
  if(curl_strequal("policy", str))
    return CURLGSSAPI_DELEGATION_POLICY_FLAG;
  if(curl_strequal("always", str))
    return CURLGSSAPI_DELEGATION_FLAG;

  warnf(config->global, "unrecognized delegation method '%s', using none",
        str);

  return CURLGSSAPI_DELEGATION_NONE;
}

/*
 * my_useragent: returns allocated string with default user agent
 */
static char *my_useragent(void)
{
  return strdup(CURL_NAME "/" CURL_VERSION);
}

#define isheadersep(x) ((((x)==':') || ((x)==';')))

/*
 * inlist() returns true if the given 'checkfor' header is present in the
 * header list.
 */
static bool inlist(const struct curl_slist *head,
                   const char *checkfor)
{
  size_t thislen = strlen(checkfor);
  DEBUGASSERT(thislen);
  DEBUGASSERT(checkfor[thislen-1] != ':');

  for(; head; head = head->next) {
    if(curl_strnequal(head->data, checkfor, thislen) &&
       isheadersep(head->data[thislen]) )
      return TRUE;
  }

  return FALSE;
}

CURLcode get_args(struct OperationConfig *config, const size_t i)
{
  CURLcode result = CURLE_OK;
  bool last = (config->next ? FALSE : TRUE);

  if(config->jsoned) {
    ParameterError err = PARAM_OK;
    /* --json also implies json Content-Type: and Accept: headers - if
       they are not set with -H */
    if(!inlist(config->headers, "Content-Type"))
      err = add2list(&config->headers, "Content-Type: application/json");
    if(!err && !inlist(config->headers, "Accept"))
      err = add2list(&config->headers, "Accept: application/json");
    if(err)
      return CURLE_OUT_OF_MEMORY;
  }

  /* Check we have a password for the given host user */
  if(config->userpwd && !config->oauth_bearer) {
    result = checkpasswd("host", i, last, &config->userpwd);
    if(result)
      return result;
  }

  /* Check we have a password for the given proxy user */
  if(config->proxyuserpwd) {
    result = checkpasswd("proxy", i, last, &config->proxyuserpwd);
    if(result)
      return result;
  }

  /* Check we have a user agent */
  if(!config->useragent) {
    config->useragent = my_useragent();
    if(!config->useragent) {
      errorf(config->global, "out of memory");
      result = CURLE_OUT_OF_MEMORY;
    }
  }

  return result;
}

/*
 * Parse the string and modify ssl_version in the val argument. Return PARAM_OK
 * on success, otherwise a parameter error enum. ONLY ACCEPTS POSITIVE NUMBERS!
 *
 * Since this function gets called with the 'nextarg' pointer from within the
 * getparameter a lot, we must check it for NULL before accessing the str
 * data.
 */

ParameterError str2tls_max(long *val, const char *str)
{
   static struct s_tls_max {
    const char *tls_max_str;
    long tls_max;
  } const tls_max_array[] = {
    { "default", CURL_SSLVERSION_MAX_DEFAULT },
    { "1.0",     CURL_SSLVERSION_MAX_TLSv1_0 },
    { "1.1",     CURL_SSLVERSION_MAX_TLSv1_1 },
    { "1.2",     CURL_SSLVERSION_MAX_TLSv1_2 },
    { "1.3",     CURL_SSLVERSION_MAX_TLSv1_3 }
  };
  size_t i = 0;
  if(!str)
    return PARAM_REQUIRES_PARAMETER;
  for(i = 0; i < sizeof(tls_max_array)/sizeof(tls_max_array[0]); i++) {
    if(!strcmp(str, tls_max_array[i].tls_max_str)) {
      *val = tls_max_array[i].tls_max;
      return PARAM_OK;
    }
  }
  return PARAM_BAD_USE;
}
