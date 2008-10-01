#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "ftbool.h"
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
#include <plugin.h>
/// #include <ft_global.h>
#define HA_FT_MAXBYTELEN 254

#if !defined(__attribute__) && (defined(__cplusplus) || !defined(__GNUC__)  || __GNUC__ == 2 && __GNUC_MINOR__ < 8)
#define __attribute__(A)
#endif

static char* space_unicode_normalize="OFF";
static char* space_unicode_version="DEFAULT";
static char icu_unicode_version[32];
static my_bool space_rawinput = FALSE;

static void* icu_malloc(const void* context, size_t size){ return my_malloc(size,MYF(MY_WME)); }
static void* icu_realloc(const void* context, void* ptr, size_t size){ return my_realloc(ptr,size,MYF(MY_WME)); }
static void  icu_free(const void* context, void *ptr){ my_free(ptr,MYF(0)); }

static int space_parser_plugin_init(void *arg __attribute__((unused))){
#if HAVE_ICU
  char errstr[128];
  UVersionInfo versionInfo;
  u_getUnicodeVersion(versionInfo);
  u_versionToString(versionInfo, icu_unicode_version);
  
  UErrorCode ustatus=0;
  u_setMemoryFunctions(NULL, icu_malloc, icu_realloc, icu_free, &ustatus);
  if(U_FAILURE(ustatus)){
    sprintf(errstr, "u_setMemoryFunctions failed. ICU status code %d\n", ustatus);
    fputs(errstr, stderr);
    fflush(stderr);
  }
#endif
  return(0);
}

static int space_parser_plugin_deinit(void *arg __attribute__((unused))){
  return(0);
}


static int space_parser_init(MYSQL_FTPARSER_PARAM *param __attribute__((unused))){
  return(0);
}
static int space_parser_deinit(MYSQL_FTPARSER_PARAM *param __attribute__((unused))){
  return(0);
}

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

static int space_parser_parse(MYSQL_FTPARSER_PARAM *param)
{
  DBUG_ENTER("space_parser_parse");
  
  CHARSET_INFO *uc = NULL;
  CHARSET_INFO *cs = param->cs;
  char* feed = param->doc;
  size_t feed_length = (size_t)param->length;
  int feed_req_free = 0;
  
  // we do convert if it was requred to normalize.
  if(strcmp(cs->csname, "utf8")!=0 && strcmp(space_unicode_normalize, "OFF")!=0){
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
  
#if HAVE_ICU
  // normalize
  if(strcmp(space_unicode_normalize, "OFF")!=0){
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
    if(strcmp(space_unicode_version, "3.2")==0) options |= UNORM_UNICODE_3_2;
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
#endif
  
  if(uc){
    // convert from UTF-8
    int cv_length = cs->mbmaxlen * uc->cset->numchars(uc, feed, feed+feed_length);
    char* cv = my_malloc(cv_length, MYF(MY_WME));
    feed_length = str_convert(uc, feed, feed_length, cs, cv, cv_length);
    if(feed_req_free) my_free(feed,MYF(0));
    feed = cv;
    feed_req_free = 1;
  }
  
  // buffer is to be free-ed
  param->flags |= MYSQL_FTFLAGS_NEED_COPY;
  size_t talloc = 32;
  size_t tlen = 0;
  char*  tbuffer = my_malloc(talloc, MYF(MY_WME));
  // Current myisam does not copy buffer. So, we'll alloc huge memory here.
  // If param->mode==MYSQL_FTPARSER_WITH_STOPWORDS, don't reuse the buffer
  // even if param->flags has MYSQL_FTFLAGS_NEED_COPY, as ftb_phrase_add_word
  // actually does not copy. We can spare memory when it is fixed.
  
  if(param->mode == MYSQL_FTPARSER_FULL_BOOLEAN_INFO){
    MYSQL_FTPARSER_BOOLEAN_INFO bool_info_may    ={ FT_TOKEN_WORD, 0, 0, 0, 0, ' ', 0 };
    MYSQL_FTPARSER_BOOLEAN_INFO instinfo;
    int depth=0;
    MYSQL_FTPARSER_BOOLEAN_INFO baseinfos[16];
    instinfo = baseinfos[0] = bool_info_may;
    
    int context=CTX_CONTROL;
    SEQFLOW sf,sf_prev = SF_BROKEN;
    char *pos=feed;
    while(pos < feed+feed_length){
      int readsize;
      my_wc_t dst;
      sf = ctxscan(cs, pos, feed+feed_length, &dst, &readsize, context);
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
        if(sf == SF_PLUS){
          instinfo.yesno = 1;
        }
        if(sf == SF_MINUS){
          instinfo.yesno = -1;
        }
        if(sf == SF_PLUS) instinfo.weight_adjust = 1;
        if(sf == SF_MINUS) instinfo.weight_adjust = -1;
        if(sf == SF_WASIGN){
          instinfo.wasign = -1;
        }
        if(sf == SF_LEFT_PAREN){
          depth++;
          if(depth>16) depth=16;
          baseinfos[depth] = instinfo;
          instinfo.type = FT_TOKEN_LEFT_PAREN;
          param->mysql_add_word(param, pos, 0, &instinfo); // push LEFT_PAREN token
          instinfo = baseinfos[depth];
        }
        if(sf == SF_RIGHT_PAREN){
          instinfo.type = FT_TOKEN_LEFT_PAREN;
          param->mysql_add_word(param, pos, 0, &instinfo); // push RIGHT_PAREN token
          depth--;
          if(depth<0) depth=0;
          instinfo = baseinfos[depth];
        }
        if(sf == SF_QUOTE_START){
          context |= CTX_QUOTE;
          depth++;
          if(depth>16) depth=16;
          instinfo.quot = (char*)1; // will be in quote
          baseinfos[depth] = instinfo; // save
          instinfo.type = FT_TOKEN_LEFT_PAREN;
          param->mysql_add_word(param, pos, 0, &instinfo); // push LEFT_PAREN token
          instinfo = baseinfos[depth]; // restore
        }
        if(context & CTX_QUOTE){
          if(my_isspace(cs, *pos) && sf_prev!=SF_ESCAPE){ // perform phrase query.
            sf = SF_WHITE;
          }
        }
        if(sf == SF_WHITE || sf == SF_QUOTE_END || sf == SF_LEFT_PAREN || sf == SF_RIGHT_PAREN || sf == SF_TRUNC){
          if(sf_prev == SF_CHAR){
            if(sf == SF_TRUNC){
              instinfo.trunc = 1;
            }
            if(tlen>0 && tlen< HA_FT_MAXBYTELEN ){ // we must not exceed HA_FT_MAXBYTELEN-HA_FT_WLEN
              param->mysql_add_word(param, tbuffer, tlen, &instinfo); // emit
            }
            tlen = 0;
            instinfo = baseinfos[depth];
          }
        }
        if(sf == SF_QUOTE_END){
          context &= ~CTX_QUOTE;
          instinfo = baseinfos[depth];
          instinfo.type = FT_TOKEN_RIGHT_PAREN;
          param->mysql_add_word(param, pos, 0, &instinfo); // push RIGHT_PAREN token
          depth--;
          if(depth<0) depth=0;
          instinfo = baseinfos[depth];
        }
        
        if(sf == SF_CHAR){
          if(tlen+readsize>talloc){
            talloc=tlen+readsize;
            tbuffer=my_realloc(tbuffer, talloc, MYF(MY_WME));
          }
          memcpy(tbuffer+tlen, pos, readsize);
          tlen += readsize;
        }else if(sf != SF_ESCAPE){
          tlen = 0;
        }
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
      if(tlen>0 && tlen < HA_FT_MAXBYTELEN){ // we must not exceed HA_FT_MAXBYTELEN-HA_FT_WLEN
        param->mysql_add_word(param, tbuffer, tlen, &instinfo); // emit
      }
    }
    if(instinfo.quot){ // quote must be closed, otherwise, MyISAM will crash.
      instinfo.type = FT_TOKEN_RIGHT_PAREN;
      param->mysql_add_word(param, pos, 0, &instinfo); // push RIGHT_PAREN token
    }
  }else{
    // Natural mode query / Indexing
    // or MYSQL_FTPARSER_WITH_STOPWORDS
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
      sf = ctxscan(cs, pos, docend, &dst, &readsize, context);
      if(readsize <= 0) break; //ILSEQ
      
      if(space_rawinput==TRUE && sf==SF_ESCAPE) sf = SF_CHAR; // cancel the effect of escaping
      
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
      // escape or space or char
      if(sf!=SF_ESCAPE){
        if(sf_prev==SF_CHAR && sf==SF_WHITE){
          if(tlen>0 && tlen < HA_FT_MAXBYTELEN){ // we must not exceed HA_FT_MAXBYTELEN-HA_FT_WLEN
            param->mysql_add_word(param, tbuffer, tlen, NULL);
          }
          tlen=0;
        }
        if(sf==SF_CHAR){
          if(tlen+readsize>talloc){
            talloc=tlen+readsize;
            tbuffer=my_realloc(tbuffer, talloc, MYF(MY_WME));
          }
          memcpy(tbuffer+tlen, pos, readsize);
          tlen += readsize;
        }
        sf_prev = sf;
      }
      pos += readsize;
    }
    if(sf==SF_CHAR){
      if(tlen>0 && tlen < HA_FT_MAXBYTELEN){ // we must not exceed HA_FT_MAXBYTELEN-HA_FT_WLEN
        param->mysql_add_word(param, tbuffer, tlen, NULL);
      }
    }
  }
  my_free(tbuffer, MYF(0)); // free-ed in deinit
  if(feed_req_free) my_free(feed,MYF(0));
  
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
  {"ICU_unicode_version", (char *)icu_unicode_version, SHOW_CHAR},
  {0,0,0}
};

static struct st_mysql_sys_var* space_system_variables[]= {
  MYSQL_SYSVAR(rawinput),
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
  0x0013,                     /* version                         */
  space_status,               /* status variables                */
  space_system_variables,     /* system variables                */
  NULL
}
mysql_declare_plugin_end;

