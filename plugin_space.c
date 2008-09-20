#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "ftbool.h"
#if HAVE_ICU
#include "ftnorm.h"
#endif

// mysql headers
#include <my_global.h>
#include <m_ctype.h>
#include <my_sys.h>
#include <plugin.h>

#if !defined(__attribute__) && (defined(__cplusplus) || !defined(__GNUC__)  || __GNUC__ == 2 && __GNUC_MINOR__ < 8)
#define __attribute__(A)
#endif

static char* space_unicode_normalize;
static my_bool space_rawinput = FALSE;

static int space_parser_plugin_init(void *arg __attribute__((unused)))
{
  return(0);
}
static int space_parser_plugin_deinit(void *arg __attribute__((unused)))
{
  return(0);
}


static int space_parser_init(MYSQL_FTPARSER_PARAM *param __attribute__((unused)))
{
  return(0);
}
static int space_parser_deinit(MYSQL_FTPARSER_PARAM *param __attribute__((unused)))
{
  return(0);
}

static size_t str_convert(CHARSET_INFO *cs, char *from, int from_length,
                          CHARSET_INFO *uc, char *to,   int to_length){
  char *rpos, *rend, *wpos, *wend;
  my_charset_conv_mb_wc mb_wc = cs->cset->mb_wc;
  my_charset_conv_wc_mb wc_mb = uc->cset->wc_mb;
  my_wc_t wc;
  
  rpos = from;
  rend = from + from_length;
  wpos = to;
  wend = to + to_length;
  while(rpos < rend){
    int cnvres = 0;
    cnvres = (*mb_wc)(cs, &wc, (uchar*)rpos, (uchar*)rend);
    if(cnvres > 0){
      rpos += cnvres;
    }else if(cnvres == MY_CS_ILSEQ){
      rpos++;
      wc = '?';
    }else if(cnvres > MY_CS_TOOSMALL){
      rpos += (-cnvres);
      wc = '?';
    }else{
      break;
    }
    cnvres = (*wc_mb)(uc, wc, (uchar*)wpos, (uchar*)wend);
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
  CHARSET_INFO *uc = NULL;
  CHARSET_INFO *cs = param->cs;
  char* feed = param->doc;
  size_t feed_length = (size_t)param->length;
  
  uint mblen;
  char* cv;
  size_t cv_length=0;
  
  if(strcmp(cs->csname, "utf8")!=0 && strcmp(space_unicode_normalize, "OFF")!=0){
    uc = get_charset(33,MYF(0)); // my_charset_utf8_general_ci for utf8 conversion
  }
  
  // convert into UTF-8
  if(uc){
    // calculate mblen and malloc.
    mblen = uc->mbmaxlen * cs->cset->numchars(cs, feed, feed+feed_length);
    cv = my_malloc(mblen, MYF(MY_WME));
    cv_length = mblen;
    feed_length = str_convert(cs, feed, feed_length, uc, cv, cv_length);
    feed = cv;
  }
  
#if HAVE_ICU
  // normalize
  if(strcmp(space_unicode_normalize, "OFF")!=0){
    char* nm;
    size_t nm_length=0;
    size_t nm_used=0;
    nm_length = feed_length+32;
    nm = my_malloc(nm_length, MYF(MY_WME));
    int status = 0;
    int mode = 1;
    if(strcmp(space_unicode_normalize, "C")==0) mode = 4;
    if(strcmp(space_unicode_normalize, "D")==0) mode = 2;
    if(strcmp(space_unicode_normalize, "KC")==0) mode = 5;
    if(strcmp(space_unicode_normalize, "KD")==0) mode = 3;
    if(strcmp(space_unicode_normalize, "FCD")==0) mode = 6;
    nm = uni_normalize(feed, feed_length, nm, nm_length, &nm_used, mode, &status);
    if(status < 0){
       nm_length=nm_used;
       nm = my_realloc(nm, nm_length, MYF(MY_WME));
       nm = uni_normalize(feed, feed_length, nm, nm_length, &nm_used, mode, &status);
    }
    if(cv_length){
      cv = my_realloc(cv, nm_used, MYF(MY_WME));
    }else{
      cv = my_malloc(nm_used, MYF(MY_WME));
    }
    memcpy(cv, nm, nm_used);
    cv_length = nm_used;
    my_free(nm,MYF(0));
    feed = cv;
    feed_length = cv_length;
  }
#endif
  
  if(uc){
    // convert from UTF-8
    mblen = cs->mbmaxlen * uc->cset->numchars(uc, feed, feed+feed_length);
    if(cv_length){
      cv = my_realloc(cv, mblen, MYF(MY_WME));
    }else{
      cv = my_malloc(mblen, MYF(MY_WME));
    }
    feed_length = str_convert(uc, feed, feed_length, cs, cv, cv_length);
    feed = cv;
  }
  
  // buffer is to be free-ed
  param->flags = MYSQL_FTFLAGS_NEED_COPY;
  size_t tlen=0;
  size_t talloc=512;
  char *tmpbuffer;
  tmpbuffer = my_malloc(talloc, MYF(MY_WME));
  
  int qmode = param->mode;
  if(qmode == MYSQL_FTPARSER_FULL_BOOLEAN_INFO){
    MYSQL_FTPARSER_BOOLEAN_INFO bool_info_may ={ FT_TOKEN_WORD, 0, 0, 0, 0, ' ', 0 };
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
        if(sf == SF_QUOTE_START) context |= CTX_QUOTE;
        if(sf == SF_QUOTE_END){
          context &= ~CTX_QUOTE;
          param->mode = MYSQL_FTPARSER_FULL_BOOLEAN_INFO;
        }
        if(sf == SF_LEFT_PAREN){
          instinfo = baseinfos[depth];
          depth++;
          if(depth>16) depth=16;
          baseinfos[depth] = instinfo;
          instinfo.type = FT_TOKEN_LEFT_PAREN;
          param->mysql_add_word(param, pos, 0, &instinfo); // push LEFT_PAREN token
        }
        if(sf == SF_RIGHT_PAREN){
          instinfo.type = FT_TOKEN_RIGHT_PAREN;
          param->mysql_add_word(param, pos, 0, &instinfo); // push RIGHT_PAREN token
          depth--;
          if(depth<0) depth=0;
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
      }
      if(context & CTX_QUOTE){
        if(my_isspace(cs, *pos)){
          param->mode = MYSQL_FTPARSER_WITH_STOPWORDS;
          sf = SF_WHITE;
        }
      }
      if(sf == SF_WHITE || sf == SF_QUOTE_END || sf == SF_LEFT_PAREN || sf == SF_RIGHT_PAREN || sf == SF_TRUNC){
        if(sf_prev == SF_CHAR){
          if(sf == SF_TRUNC){
            instinfo.trunc = 1;
          }
          param->mysql_add_word(param, tmpbuffer, tlen, &instinfo); // emit
        }
        instinfo = baseinfos[depth];
      }
      if(sf == SF_CHAR){
        if(tlen+readsize>talloc){
          talloc=tlen+readsize;
          tmpbuffer=my_realloc(tmpbuffer, talloc, MYF(MY_WME));
        }
        memcpy(tmpbuffer+tlen, pos, readsize);
        tlen += readsize;
      }else if(sf != SF_ESCAPE){
        tlen = 0;
      }
      
      if(readsize > 0){
        pos += readsize;
      }else if(readsize == MY_CS_ILSEQ){
        pos++;
      }else if(readsize > MY_CS_TOOSMALL){
        pos += (-readsize);
      }else{
        break;
      }
      sf_prev = sf;
    }
    if(sf==SF_CHAR){
      param->mysql_add_word(param, tmpbuffer, tlen, &instinfo); // emit
    }
  }else{
    // Natural mode query / Indexing
    SEQFLOW sf,sf_prev = SF_BROKEN;
    int context=CTX_CONTROL;
    int isspace_prev=1, isspace_cur=0; // boolean
    int mbunit=1;
    
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
        if(sf_prev==SF_WHITE && sf==SF_CHAR){
          tlen=0;
        }
        if(sf_prev==SF_CHAR && sf==SF_WHITE){
          param->mysql_add_word(param, tmpbuffer, tlen, NULL);
          tlen=0;
        }
        if(sf==SF_CHAR){
          if(tlen+readsize>talloc){
            talloc=tlen+readsize;
            tmpbuffer=my_realloc(tmpbuffer, talloc, MYF(MY_WME));
          }
          memcpy(tmpbuffer+tlen, pos, readsize);
          tlen+=readsize;
        }
        sf_prev = sf;
      }
      pos += readsize;
    }
    if(sf==SF_CHAR){
      param->mysql_add_word(param, tmpbuffer, tlen, NULL);
    }
  }
  my_free(tmpbuffer,MYF(0));
  if(cv_length>0) my_free(cv, MYF(0));
  return(0);
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

static struct st_mysql_sys_var* space_system_variables[]= {
  MYSQL_SYSVAR(rawinput),
#if HAVE_ICU
  MYSQL_SYSVAR(normalization),
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
  0x0010,                     /* version                         */
  NULL,                       /* status variables                */
  space_system_variables,     /* system variables                */
  NULL
}
mysql_declare_plugin_end;

