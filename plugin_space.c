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


static int space_parser_parse(MYSQL_FTPARSER_PARAM *param)
{
  CHARSET_INFO *cs = param->cs;
  char* feed = param->doc;
  size_t feed_length = (size_t)param->length;
  
  char* nm;
  size_t nm_length=0;
  size_t nm_used=0;
  
#if HAVE_ICU
  if(strcmp(cs->csname,"utf8")==0 && strcmp(space_unicode_normalize, "NONE")!=0){
    nm_length = param->length+32;
    nm = my_malloc(nm_length, MYF(MY_WME));
    int mode = 1;
    if(strcmp(space_unicode_normalize, "C")==0) mode = 4;
    if(strcmp(space_unicode_normalize, "D")==0) mode = 2;
    if(strcmp(space_unicode_normalize, "KC")==0) mode = 5;
    if(strcmp(space_unicode_normalize, "KD")==0) mode = 3;
    if(strcmp(space_unicode_normalize, "FCD")==0) mode = 6;
    if(uni_normalize(param->doc, param->length, nm, nm_length, &nm_used, mode)!=0){
       nm_length=nm_used;
       nm = my_realloc(nm, nm_length, MYF(MY_WME));
       uni_normalize(param->doc, param->length, nm, nm_length, &nm_used, mode);
    }
    feed = nm;
    feed_length = nm_used;
  }
#endif
  
  // buffer is to be free-ed
  param->flags = MYSQL_FTFLAGS_NEED_COPY;
  if(param->mode == MYSQL_FTPARSER_FULL_BOOLEAN_INFO){
    MYSQL_FTPARSER_BOOLEAN_INFO bool_info_may ={ FT_TOKEN_WORD, 0, 0, 0, 0, ' ', 0 };
    MYSQL_FTPARSER_BOOLEAN_INFO instinfo;
    int depth=0;
    MYSQL_FTPARSER_BOOLEAN_INFO baseinfos[16];
    instinfo = baseinfos[0] = bool_info_may;
    
    size_t tlen=0;
    char *tmpbuffer;
    tmpbuffer = my_malloc(feed_length, MYF(MY_WME)); // TODO: This allocates too much memory. shrink later.
    
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
        if(sf == SF_QUOTE_END)   context &= ~CTX_QUOTE;
        if(sf == SF_LEFT_PAREN){
          instinfo = baseinfos[depth];
          depth++;
          if(depth>16) depth=16;
          baseinfos[depth] = instinfo;
          instinfo.type = FT_TOKEN_LEFT_PAREN;
          param->mysql_add_word(param, pos, 0, &instinfo);
        }
        if(sf == SF_RIGHT_PAREN){
          instinfo.type = FT_TOKEN_RIGHT_PAREN;
          param->mysql_add_word(param, pos, 0, &instinfo);
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
    
    my_free(tmpbuffer,MYF(0));
  }else{
    // Natural mode query / Indexing
    int isspace_prev=1, isspace_cur=0; // boolean
    int mbunit=1;
    
    char *pos, *wstart, *end;
    pos = wstart = feed;
    end = feed + feed_length;
    while(pos < end){
      isspace_cur = my_isspace(cs, *pos);
      mbunit  = param->cs->cset->mbcharlen(cs, *(uchar *)pos);
      if(mbunit <= 0) break; // ILSEQ
      
      if(isspace_prev && !isspace_cur){
        wstart = pos;
      }
      if(!isspace_prev && isspace_cur){
        param->mysql_add_word(param, wstart, pos+mbunit-wstart, NULL);
        wstart = pos+mbunit;
      }
      isspace_prev = isspace_cur;
      pos += mbunit;
    }
    if(!isspace_prev && wstart < end){
      param->mysql_add_word(param, wstart, end-wstart, NULL);
    }
  }
  if(nm_used>0) my_free(nm, MYF(0));
  return(0);
}

int space_unicode_normalize_check(MYSQL_THD thd, struct st_mysql_sys_var *var, void *save, struct st_mysql_value *value){
    char buf[4];
    int len=4;
    const char *str;
    
    str = value->val_str(value,buf,&len);
    if(!str) return -1;
    if(len==1){
        if(str[0]=='C'){ *(const char**)save=str; return 0;}
        if(str[0]=='D'){ *(const char**)save=str; return 0;}
    }
    if(len==2){
        if(str[0]=='K' && str[1]=='C'){ *(const char**)save=str; return 0;}
        if(str[0]=='K' && str[1]=='D'){ *(const char**)save=str; return 0;}
    }
    if(len==3){
        if(str[0]=='F' && str[1]=='C' && str[2]=='D'){ *(const char**)save=str; return 0;}
    }
    if(len==4){
        if(str[0]=='N' && str[1]=='O' && str[2]=='N' && str[3]=='E'){ *(const char**)save=str; return 0;}
    }
    return -1;
}

static MYSQL_SYSVAR_STR(unicode_normalize, space_unicode_normalize,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
  "Set unicode normalization (NONE, C, D, KC, KD, FCD)",
  space_unicode_normalize_check, NULL, "NONE");

static struct st_mysql_sys_var* space_system_variables[]= {
#if HAVE_ICU
  MYSQL_SYSVAR(unicode_normalize),
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

