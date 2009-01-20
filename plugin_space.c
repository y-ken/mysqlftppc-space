#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "ftbool.h"
#include "ftstring.h"
#if HAVE_ICU
#include <unicode/uclean.h>
#include <unicode/uversion.h>
#include <unicode/uchar.h>
#include <unicode/unorm.h>
#include "ftnorm.h"
#endif

// mysql headers
#include <my_global.h>
#include <m_ctype.h>
#include <my_sys.h>
#include <my_list.h>
#include <plugin.h>
/// #include <ft_global.h>
#define HA_FT_MAXBYTELEN 254

#if !defined(__attribute__) && (defined(__cplusplus) || !defined(__GNUC__)  || __GNUC__ == 2 && __GNUC_MINOR__ < 8)
#define __attribute__(A)
#endif

static char* space_unicode_normalize = NULL;
static char* space_unicode_version = NULL;
static char space_info[128];
static my_bool space_rawinput = FALSE;
static my_bool space_drop_long_token = FALSE;

static void* icu_malloc(const void* context, size_t size){ return my_malloc(size,MYF(MY_WME)); }
static void* icu_realloc(const void* context, void* ptr, size_t size){ return my_realloc(ptr,size,MYF(MY_WME)); }
static void  icu_free(const void* context, void *ptr){ my_free(ptr,MYF(0)); }

static int space_parser_parse_boolean(MYSQL_FTPARSER_PARAM *param, char* feed, int feed_length, int feed_req_free);
static int space_parser_parse_natural(MYSQL_FTPARSER_PARAM *param, char* feed, int feed_length, int feed_req_free);

/** ftstate */
static LIST* list_top(LIST* root){
  LIST *cur = root;
  while(cur && cur->next){
    cur = cur->next;
  }
  return cur;
}

struct ftppc_mem_bulk {
  void*  mem_head;
  void*  mem_cur;
  size_t mem_size;
};

struct ftppc_state {
  /** immutable memory buffer */
  void*  engine;
  size_t bulksize;
  LIST*  mem_root;
};

static void* ftppc_alloc(struct ftppc_state *state, size_t length){
  LIST *cur = list_top(state->mem_root);
  while(1){
    if(!cur){
      // hit the root. create a new bulk.
      size_t sz = state->bulksize<<1;
      while(sz < length){
        sz = sz<<1;
      }
      state->bulksize = sz;
      
      struct ftppc_mem_bulk *tmp = (struct ftppc_mem_bulk*)my_malloc(sizeof(struct ftppc_mem_bulk), MYF(MY_WME));
      tmp->mem_head = my_malloc(sz, MYF(MY_WME));
      tmp->mem_cur  = tmp->mem_head;
      tmp->mem_size = sz;
      
      state->mem_root = list_cons(tmp, cur);
      cur = state->mem_root;
    }
    
    struct ftppc_mem_bulk *bulk = (struct ftppc_mem_bulk*)cur->data;
    
    if(bulk->mem_cur + length < bulk->mem_head + bulk->mem_size){
      void* addr = bulk->mem_cur;
      bulk->mem_cur += length;
      return addr;
    }
    cur = cur->prev;
  }
}
/** /ftstate */

static int space_parser_plugin_init(void *arg __attribute__((unused))){
  space_info[0]='\0';
#if HAVE_ICU
  char icu_tmp_str[16];
  char errstr[128];
  UVersionInfo versionInfo;
  u_getVersion(versionInfo); // get ICU version
  u_versionToString(versionInfo, icu_tmp_str);
  strcat(space_info, "with ICU ");
  strcat(space_info, icu_tmp_str);
  u_getUnicodeVersion(versionInfo); // get ICU Unicode version
  u_versionToString(versionInfo, icu_tmp_str);
  strcat(space_info, "(Unicode ");
  strcat(space_info, icu_tmp_str);
  strcat(space_info, ")");
  
  UErrorCode ustatus=0;
  u_setMemoryFunctions(NULL, icu_malloc, icu_realloc, icu_free, &ustatus);
  if(U_FAILURE(ustatus)){
    sprintf(errstr, "u_setMemoryFunctions failed. ICU status code %d\n", ustatus);
    fputs(errstr, stderr);
    fflush(stderr);
  }
#else
  strcat(space_info, "without ICU");
#endif
  return(0);
}

static int space_parser_plugin_deinit(void *arg __attribute__((unused))){
  return(0);
}


static int space_parser_init(MYSQL_FTPARSER_PARAM *param __attribute__((unused))){
  struct ftppc_state *state = (struct ftppc_state*)my_malloc(sizeof(struct ftppc_state), MYF(MY_WME));
  state->engine=NULL;
  state->bulksize=8;
  state->mem_root=NULL;
  param->ftparser_state = state;
  return(0);
}
static int space_parser_deinit(MYSQL_FTPARSER_PARAM *param __attribute__((unused))){
  list_free(((struct ftppc_state*)param->ftparser_state)->mem_root, 1);
  return(0);
}

#if HAVE_ICU
static size_t str_convert(CHARSET_INFO *cs, char *from, size_t from_length,
                          CHARSET_INFO *uc, char *to,   size_t to_length){
  char *rpos, *rend, *wpos, *wend;
  my_wc_t wc;
  
  rpos = from;
  rend = from + from_length;
  wpos = to;
  wend = to + to_length;
  while(rpos < rend){
    int cnvres = 0;
    cnvres = cs->cset->mb_wc(cs, &wc, (uchar*)rpos, (uchar*)rend);
    if(cnvres > 0){
      rpos += cnvres;
    }else if(cnvres == MY_CS_ILSEQ){
      rpos++;
      wc = '?';
    }else{
      break;
    }
    cnvres = uc->cset->wc_mb(uc, wc, (uchar*)wpos, (uchar*)wend);
    if(cnvres > 0){
      wpos += cnvres;
    }else{
      break;
    }
  }
  return (size_t)(wpos - to);
}
#endif

static int space_parser_parse(MYSQL_FTPARSER_PARAM *param)
{
  DBUG_ENTER("space_parser_parse");
  
  char* feed = param->doc;
  size_t feed_length = (size_t)param->length;
  int feed_req_free = 0;
  CHARSET_INFO *cs = param->cs;
#if HAVE_ICU
  CHARSET_INFO *uc = NULL;
  
  // we do convert if it was requred to normalize.
  if(strcmp(cs->csname, "utf8")!=0 && space_unicode_normalize && strcmp(space_unicode_normalize, "OFF")!=0){
    uc = get_charset(33,MYF(0)); // my_charset_utf8_general_ci for utf8 conversion
  }
  
  // convert into UTF-8
  if(uc){
    char* cv;
    size_t cv_length=0;
    // calculate mblen and malloc.
    cv_length = uc->mbmaxlen * cs->cset->numchars(cs, feed, feed+feed_length);
    cv = my_malloc(cv_length, MYF(MY_WME));
    feed_length = str_convert(cs, feed, feed_length, uc, cv, cv_length);
    feed = cv;
    feed_req_free = 1;
  }
  
  // normalize
  if(space_unicode_normalize && strcmp(space_unicode_normalize, "OFF")!=0){
    char* nm;
    char* t;
    size_t nm_length=0;
    size_t nm_used=0;
    nm_length = feed_length+32;
    nm = my_malloc(nm_length, MYF(MY_WME));
    int status = 0;
    int mode = UNORM_NONE;
    int options = 0;
    if(strcmp(space_unicode_normalize, "C")==0) mode = UNORM_NFC;
    if(strcmp(space_unicode_normalize, "D")==0) mode = UNORM_NFD;
    if(strcmp(space_unicode_normalize, "KC")==0) mode = UNORM_NFKC;
    if(strcmp(space_unicode_normalize, "KD")==0) mode = UNORM_NFKD;
    if(strcmp(space_unicode_normalize, "FCD")==0) mode = UNORM_FCD;
    if(space_unicode_version && strcmp(space_unicode_version, "3.2")==0) options |= UNORM_UNICODE_3_2;
    t = uni_normalize(feed, feed_length, nm, nm_length, &nm_used, mode, options, &status);
    if(status != 0){
      nm_length=nm_used;
      nm = my_realloc(nm, nm_length, MYF(MY_WME));
      t = uni_normalize(feed, feed_length, nm, nm_length, &nm_used, mode, options, &status);
      if(status != 0){
        fputs("unicode normalization failed.\n",stderr);
        fflush(stderr);
      }else{
        nm = t;
      }
    }else{
      nm = t;
    }
    feed_length = nm_used;
    if(feed_req_free) my_free(feed,MYF(0));
    feed = nm;
    feed_req_free = 1;
  }
  
  if(uc){
    // convert from UTF-8
    int cv_length = cs->mbmaxlen * uc->cset->numchars(uc, feed, feed+feed_length);
    char* cv = my_malloc(cv_length, MYF(MY_WME));
    feed_length = str_convert(uc, feed, feed_length, cs, cv, cv_length);
    if(feed_req_free) my_free(feed,MYF(0));
    feed = cv;
    feed_req_free = 1;
  }
#endif
  
  if(param->mode == MYSQL_FTPARSER_FULL_BOOLEAN_INFO){
    space_parser_parse_boolean(param, feed, feed_length, feed_req_free);
  }else{
    // space plugin does not have stop-words.
    space_parser_parse_natural(param, feed, feed_length, feed_req_free);
  }
  if(feed_req_free) my_free(feed,MYF(0));
  
  DBUG_RETURN(0);
}

static int space_parser_parse_boolean(MYSQL_FTPARSER_PARAM *param, char* feed, int feed_length, int feed_req_free){
  DBUG_ENTER("space_parser_parse_boolean");
  
  FTSTRING buffer = {NULL, 0, NULL, 0, 0};
  FTSTRING *pbuffer = &buffer;
  
  ftstring_bind(pbuffer, feed, feed_req_free);
  
  MYSQL_FTPARSER_BOOLEAN_INFO info_may = { FT_TOKEN_WORD, 0, 0, 0, 0, ' ', 0 }; // root boolean_info
  MYSQL_FTPARSER_BOOLEAN_INFO instinfo = info_may; // boolean_info of cursor
  LIST infos_root = { NULL, NULL, NULL };
  LIST *infos;
  infos = &infos_root;
  infos->data = &info_may;
  
  int context=CTX_CONTROL;
  SEQFLOW sf,sf_prev = SF_BROKEN;
  char *pos=feed;
  while(pos < feed+feed_length){
    int readsize;
    my_wc_t dst;
    sf = ctxscan(param->cs, pos, feed+feed_length, &dst, &readsize, context);
    if(sf==SF_ESCAPE){
      context |= CTX_ESCAPE;
      context |= CTX_CONTROL;
    }else{
      context &= ~CTX_ESCAPE;
      if(sf == SF_CHAR){
        context &= ~CTX_CONTROL;
      }else{
        context |= CTX_CONTROL;
      }
    }
    
    if(context & CTX_QUOTE){
      if(my_isspace(param->cs, *pos) && sf_prev!=SF_ESCAPE){ // perform phrase query.
        sf = SF_WHITE;
      }
    }
    if(sf != SF_CHAR && sf != SF_ESCAPE){
      if(sf_prev == SF_CHAR){
        if(sf == SF_TRUNC){
          instinfo.trunc = 1;
        }
        int tlen = ftstring_length(pbuffer);
        char* thead = ftstring_head(pbuffer);
        if(tlen > 0){
          if(ftstring_internal(pbuffer)){
            thead = (char*)ftppc_alloc((struct ftppc_state*)param->ftparser_state, tlen);
            memcpy(thead, ftstring_head(pbuffer), tlen);
          }
          if(tlen < HA_FT_MAXBYTELEN){
            param->mysql_add_word(param, thead, tlen, &instinfo);
//             
//             char buf[1024];
//             memcpy(buf, ftstring_head(pbuffer), tlen);
//             buf[tlen]='\0';
//             fputs(buf, stderr);
//             fputs("\n", stderr);
//             fflush(stderr);
          }else{
            if(space_drop_long_token==FALSE){
              param->mysql_add_word(param, ftstring_head(pbuffer), HA_FT_MAXBYTELEN, &instinfo);
            }else{
              // we should raise warn here.
            }
          }
          param->flags = 0;
        }
        ftstring_reset(pbuffer);
        instinfo = *(MYSQL_FTPARSER_BOOLEAN_INFO *)infos->data;
      }
    }
    if(sf == SF_PLUS){   instinfo.yesno = 1; }
    if(sf == SF_MINUS){  instinfo.yesno = -1; }
    if(sf == SF_STRONG){ instinfo.weight_adjust++; }
    if(sf == SF_WEAK){   instinfo.weight_adjust--; }
    if(sf == SF_WASIGN){ instinfo.wasign = !instinfo.wasign; }
    if(sf == SF_LEFT_PAREN){
      MYSQL_FTPARSER_BOOLEAN_INFO *tmp = (MYSQL_FTPARSER_BOOLEAN_INFO*)my_malloc(sizeof(MYSQL_FTPARSER_BOOLEAN_INFO), MYF(MY_WME));
      *tmp = instinfo;
      list_push(infos, tmp);
      
      instinfo.type = FT_TOKEN_LEFT_PAREN;
      param->mysql_add_word(param, pos, 0, &instinfo); // push LEFT_PAREN token
      instinfo = *tmp;
    }
    if(sf == SF_QUOTE_START){
      context |= CTX_QUOTE;
      instinfo.quot = (char*)1; // will be in quote
      
      MYSQL_FTPARSER_BOOLEAN_INFO *tmp = (MYSQL_FTPARSER_BOOLEAN_INFO*)my_malloc(sizeof(MYSQL_FTPARSER_BOOLEAN_INFO), MYF(MY_WME));
      *tmp = instinfo;
      list_push(infos, tmp);
      
      instinfo.type = FT_TOKEN_LEFT_PAREN;
      param->mysql_add_word(param, pos, 0, &instinfo); // push LEFT_PAREN token
      instinfo = *tmp;
    }
    if(sf == SF_RIGHT_PAREN){
      instinfo = *((MYSQL_FTPARSER_BOOLEAN_INFO*)infos->data);
      instinfo.type = FT_TOKEN_RIGHT_PAREN;
      param->mysql_add_word(param, pos, 0, &instinfo); // push RIGHT_PAREN token
      
      MYSQL_FTPARSER_BOOLEAN_INFO *tmp = infos->data;
      if(tmp){
        if(tmp != &info_may){
          my_free(tmp, MYF(0));
        }else{
          // bad boolean syntax
        }
      }
      list_pop(infos);
      instinfo = *((MYSQL_FTPARSER_BOOLEAN_INFO*)infos->data);
    }
    if(sf == SF_QUOTE_END){
      context &= ~CTX_QUOTE;
      
      instinfo = *((MYSQL_FTPARSER_BOOLEAN_INFO*)infos->data);
      instinfo.type = FT_TOKEN_RIGHT_PAREN;
      instinfo.quot = (char*)1; // This is not required normally.
      param->mysql_add_word(param, pos, 0, &instinfo); // push RIGHT_PAREN token
      
      MYSQL_FTPARSER_BOOLEAN_INFO *tmp = infos->data;
      if(tmp){
        if(tmp != &info_may){
          my_free(tmp, MYF(0));
        }else{
          // bad boolean syntax
        }
      }
      list_pop(infos);
      instinfo = *((MYSQL_FTPARSER_BOOLEAN_INFO*)infos->data);
    }
    if(sf == SF_CHAR){
      if(ftstring_length(pbuffer)==0){
        ftstring_bind(pbuffer, pos, feed_req_free);
      }
      ftstring_append(pbuffer, pos, readsize);
    }
    
    if(readsize > 0){
      pos += readsize;
    }else if(readsize == MY_CS_ILSEQ){
      pos++;
    }else{
      break;
    }
    sf_prev = sf;
  }
  if(sf==SF_CHAR){
    int tlen = ftstring_length(pbuffer);
    char* thead = ftstring_head(pbuffer);
    if(tlen > 0){
      if(ftstring_internal(pbuffer)){
        thead = (char*)ftppc_alloc((struct ftppc_state*)param->ftparser_state, tlen);
        memcpy(thead, ftstring_head(pbuffer), tlen);
      }
      if(tlen < HA_FT_MAXBYTELEN){
        param->mysql_add_word(param, thead, tlen, &instinfo);
//         
//         char buf[1024];
//         memcpy(buf, ftstring_head(pbuffer), tlen);
//         buf[tlen]='\0';
//         fputs(buf, stderr);
//         fputs("\n", stderr);
//         fflush(stderr);
      }else{
        if(space_drop_long_token==FALSE){
          param->mysql_add_word(param, ftstring_head(pbuffer), HA_FT_MAXBYTELEN, &instinfo);
        }else{
          // we should raise warn here.
        }
      }
      param->flags = 0;
    }
  }
  if(instinfo.quot){ // quote must be closed, otherwise, MyISAM will crash.
    instinfo.type = FT_TOKEN_RIGHT_PAREN;
    param->mysql_add_word(param, pos, 0, &instinfo); // push RIGHT_PAREN token
  }
  ftstring_destroy(pbuffer);
  DBUG_RETURN(0);
}

static int space_parser_parse_natural(MYSQL_FTPARSER_PARAM *param, char* feed, int feed_length, int feed_req_free){
  DBUG_ENTER("space_parser_parse_natural");
  
  // Natural mode query / Indexing or MYSQL_FTPARSER_WITH_STOPWORDS
  FTSTRING buffer = {NULL, 0, NULL, 0, 0};
  FTSTRING *pbuffer = &buffer;
  ftstring_bind(pbuffer, feed, feed_req_free);
  
  SEQFLOW sf,sf_prev = SF_BROKEN;
  int context=CTX_CONTROL;
  int isspace_prev=1, isspace_cur=0; // boolean
  int mbunit=1;
  
  sf = SF_WHITE;
  char* pos = feed;
  char* docend = feed + feed_length;
  while(pos < docend){
    int readsize;
    my_wc_t dst;
    sf = ctxscan(param->cs, pos, docend, &dst, &readsize, context);
    if(readsize <= 0) break; //ILSEQ
    
    if(space_rawinput==TRUE && sf==SF_ESCAPE){ sf = SF_CHAR; } // cancel the effect of escaping
    
    // update context
    if(sf==SF_ESCAPE){
      context |= CTX_ESCAPE;
      context |= CTX_CONTROL;
    }else{
      context &= ~CTX_ESCAPE;
      
      if(sf == SF_WHITE){
        context |= CTX_CONTROL;
      }else{
        context &= ~CTX_CONTROL;
        sf = SF_CHAR;
      }
    }
    
    if(sf_prev==SF_CHAR && sf==SF_WHITE){
      int tlen = ftstring_length(pbuffer);
      char* thead = ftstring_head(pbuffer);
      if(tlen > 0){
        if(ftstring_internal(pbuffer)){
          thead = (char*)ftppc_alloc((struct ftppc_state*)param->ftparser_state, tlen);
          memcpy(thead, ftstring_head(pbuffer), tlen);
        }
        if(tlen < HA_FT_MAXBYTELEN){
          param->mysql_add_word(param, thead, tlen, NULL);
//           
//           char buf[1024];
//           memcpy(buf, ftstring_head(pbuffer), tlen);
//           buf[tlen]='\0';
//           fputs(buf, stderr);
//           fputs("\n", stderr);
//           fflush(stderr);
        }else{
          if(space_drop_long_token==FALSE){
            param->mysql_add_word(param, ftstring_head(pbuffer), HA_FT_MAXBYTELEN, NULL);
          }else{
            // we should raise warn here.
          }
        }
        param->flags = 0;
      }
      ftstring_reset(pbuffer);
    }
    if(sf==SF_CHAR){
      if(ftstring_length(pbuffer)==0){
        ftstring_bind(pbuffer, pos, feed_req_free);
      }
      ftstring_append(pbuffer, pos, readsize);
    }
    sf_prev = sf;
    pos += readsize;
  }
  if(sf==SF_CHAR){
    int tlen = ftstring_length(pbuffer);
    char* thead = ftstring_head(pbuffer);
    if(tlen > 0){
      if(ftstring_internal(pbuffer)){
        thead = (char*)ftppc_alloc((struct ftppc_state*)param->ftparser_state, tlen);
        memcpy(thead, ftstring_head(pbuffer), tlen);
      }
      if(tlen < HA_FT_MAXBYTELEN){
        param->mysql_add_word(param, thead, tlen, NULL);
//         
//         char buf[1024];
//         memcpy(buf, ftstring_head(pbuffer), tlen);
//         buf[tlen]='\0';
//         fputs(buf, stderr);
//         fputs("\n", stderr);
//         fflush(stderr);
      }else{
        if(space_drop_long_token==FALSE){
          param->mysql_add_word(param, ftstring_head(pbuffer), HA_FT_MAXBYTELEN, NULL);
        }else{
          // we should raise warn here.
        }
      }
      param->flags = 0;
    }
  }
  ftstring_destroy(pbuffer);
  DBUG_RETURN(0);
}

int space_unicode_version_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value){
    char buf[4];
    int len=4;
    const char *str;
    
    str = value->val_str(value,buf,&len);
    if(!str) return -1;
    *(const char**)save=str;
    if(len==3){
      if(memcmp(str, "3.2", len)==0) return 0;
    }
    if(len==7){
      if(memcmp(str, "DEFAULT", len)==0) return 0;
    }
    return -1;
}

int space_unicode_normalize_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value){
    char buf[4];
    int len=4;
    const char *str;
    
    str = value->val_str(value,buf,&len);
    if(!str) return -1;
    *(const char**)save=str;
    if(!get_charset(33,MYF(0))) return -1; // If you don't have utf8 codec in mysql, it fails
    if(len==1){
        if(str[0]=='C'){ return 0;}
        if(str[0]=='D'){ return 0;}
    }
    if(len==2){
        if(str[0]=='K' && str[1]=='C'){ return 0;}
        if(str[0]=='K' && str[1]=='D'){ return 0;}
    }
    if(len==3){
        if(str[0]=='F' && str[1]=='C' && str[2]=='D'){ return 0;}
        if(str[0]=='O' && str[1]=='F' && str[2]=='F'){ return 0;}
    }
    return -1;
}

static MYSQL_SYSVAR_BOOL(rawinput, space_rawinput,
  PLUGIN_VAR_OPCMDARG,
  "Treat the text as a rawinput",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_BOOL(drop_long_token, space_drop_long_token,
  PLUGIN_VAR_OPCMDARG,
  "Do not index trimmed token and just drop long token.",
  NULL, NULL, FALSE);

static MYSQL_SYSVAR_STR(normalization, space_unicode_normalize,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Set unicode normalization (OFF, C, D, KC, KD, FCD)",
  space_unicode_normalize_check, NULL, "OFF");

static MYSQL_SYSVAR_STR(unicode_version, space_unicode_version,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Set unicode version (3.2, DEFAULT)",
  space_unicode_version_check, NULL, "DEFAULT");

static struct st_mysql_show_var space_status[]=
{
  {"Space_info", (char *)space_info, SHOW_CHAR},
  {0,0,0}
};

static struct st_mysql_sys_var* space_system_variables[]= {
  MYSQL_SYSVAR(rawinput),
  MYSQL_SYSVAR(drop_long_token),
#if HAVE_ICU
  MYSQL_SYSVAR(normalization),
  MYSQL_SYSVAR(unicode_version),
#endif
  NULL
};

static struct st_mysql_ftparser space_parser_descriptor=
{
  MYSQL_FTPARSER_INTERFACE_VERSION, /* interface version      */
  space_parser_parse,              /* parsing function       */
  space_parser_init,               /* parser init function   */
  space_parser_deinit              /* parser deinit function */
};

mysql_declare_plugin(ft_space)
{
  MYSQL_FTPARSER_PLUGIN,      /* type                            */
  &space_parser_descriptor,  /* descriptor                      */
  "space",                   /* name                            */
  "Hiroaki Kawai",            /* author                          */
  "space Full-Text Parser", /* description                     */
  PLUGIN_LICENSE_BSD,
  space_parser_plugin_init,  /* init function (when loaded)     */
  space_parser_plugin_deinit,/* deinit function (when unloaded) */
  0x0016,                     /* version                         */
  space_status,               /* status variables                */
  space_system_variables,     /* system variables                */
  NULL
}
mysql_declare_plugin_end;

