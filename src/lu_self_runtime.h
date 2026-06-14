/* lu_self_runtime.h — C helpers for Lu self-hosting compiler */
#define _GNU_SOURCE
#ifndef LU_SELF_RUNTIME_H
#define LU_SELF_RUNTIME_H
/* Lu argc/argv — defined in main() */
extern int   __luc_argc;
extern char **__luc_argv;

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdbool.h>

static inline char *luc_rt_strdup(const char *s) {
    if (!s) s = "";
    size_t n = strlen(s) + 1;
    char *d = (char*)malloc(n);
    if (!d) { fprintf(stderr, "out of memory\n"); exit(1); }
    memcpy(d, s, n);
    return d;
}

/* ── Token types ── */
#define TK_EOF    0
#define TK_NL     1
#define TK_INT    2
#define TK_FLT    3
#define TK_STR    4
#define TK_IDENT  5
#define TK_BOOL   6
#define TK_BLKID  7
#define TK_BLKEND 8
#define TK_IF    10
#define TK_ELIF  11
#define TK_ELSE  12
#define TK_TO    13
#define TK_PR    14
#define TK_FN    15
#define TK_CALL  16
#define TK_RET   17
#define TK_LOOP  18
#define TK_SET   19
#define TK_STRUCT 20
#define TK_ENUM  21
#define TK_PTR   22
#define TK_ALLOC 23
#define TK_FREE  24
#define TK_DCST  25
#define TK_TYINT  30
#define TK_TYFLT  31
#define TK_TYSTR  32
#define TK_TYBOOL 33
#define TK_TYVOID 34
#define TK_TYBYTE 35
#define TK_TYI64  36
#define TK_IMPORT 40
#define TK_LANG   41
#define TK_MODE   42
#define TK_PLUS   50
#define TK_MINUS  51
#define TK_STAR   52
#define TK_SLASH  53
#define TK_PCT    54
#define TK_EQ     55
#define TK_NEQ    56
#define TK_LT     57
#define TK_GT     58
#define TK_LE     59
#define TK_GE     60
#define TK_AND    61
#define TK_OR     62
#define TK_BANG   63
#define TK_ASSIGN 64
#define TK_DOT    65
#define TK_COMMA  66
#define TK_SEMI   67
#define TK_LPAR   68
#define TK_RPAR   69
#define TK_LBRC   70
#define TK_RBRC   71
#define TK_COLON  72
#define TK_LBRAK  73
#define TK_RBRAK  74
#define TK_DEFCFG 75
#define TK_BREAK  76
#define TK_CONT   77

#define LU_MAX_TOK   65536
#define LU_MAX_SRC   1048576
#define LU_MAX_STR   512

/* ── Token storage ── */
typedef struct {
    int   type;
    int   line;
    int   col;
    char *val;   /* points into src or owned heap */
    int   ival;  /* for int literals */
    int   block; /* for TK_BLKID */
} LTok;

/* ── Lexer state ── */
typedef struct {
    char  *src;
    int    slen;
    int    pos;
    int    line;
    int    col;
    LTok  *toks;
    int    ntok;
} LLex;

/* Forward declarations */
static int  ll_kw(const char *w, int wlen);
static void ll_add(LLex *l, int type, const char *val, int line, int col);

static int ll_kw(const char *w, int wlen) {
    #define KW(s,t) if(wlen==(int)strlen(s) && strncmp(w,s,wlen)==0) return t
    KW("int",    TK_TYINT);  KW("float",  TK_TYFLT);
    KW("str",    TK_TYSTR);  KW("bool",   TK_TYBOOL);
    KW("void",   TK_TYVOID); KW("byte",   TK_TYBYTE);
    KW("int64",  TK_TYI64);  KW("true",   TK_BOOL);
    KW("false",  TK_BOOL);   KW("struct", TK_STRUCT);
    KW("enum",   TK_ENUM);   KW("Import", TK_IMPORT);
    #undef KW
    /* Lu/ keywords */
    #define KS(s,t) if(strncmp(w,s,strlen(s))==0&&wlen==(int)strlen(s)) return t
    KS("If/",TK_IF); KS("Elif/",TK_ELIF); KS("Else/",TK_ELSE);
    KS("To/",TK_TO); KS("Pr/",TK_PR);     KS("Fn/",TK_FN);
    KS("Call/",TK_CALL); KS("Ret/",TK_RET); KS("Loop/",TK_LOOP);
    KS("Set/",TK_SET);   KS("Free/",TK_FREE); KS("Alloc/",TK_ALLOC);
    KS("Break/",TK_BREAK); KS("M/",TK_MODE);
    KS("ptr/",TK_PTR);
    #undef KS
    if (wlen >= 9 && strncmp(w,"Def:const",9)==0) return TK_DCST;
    if (wlen >= 10 && strncmp(w,"Def:config",10)==0) return TK_DEFCFG;
    if (wlen >= 11 && strncmp(w,"Lu/Language",11)==0) return TK_LANG;
    return TK_IDENT;
}

static void ll_add(LLex *l, int type, const char *val, int line, int col) {
    if (l->ntok >= LU_MAX_TOK) return;
    l->toks[l->ntok].type  = type;
    l->toks[l->ntok].line  = line;
    l->toks[l->ntok].col   = col;
    l->toks[l->ntok].val   = luc_rt_strdup(val ? val : "");
    l->toks[l->ntok].ival  = 0;
    l->toks[l->ntok].block = 0;
    l->ntok++;
}

/* ── Main lex function ── */
__attribute__((noinline)) static int lu_self_lex(char *src, int slen, LTok *toks) {
    LLex l = {src, slen, 0, 1, 1, toks, 0};
    char buf[LU_MAX_STR];

    while (l.pos < l.slen) {
        char c = src[l.pos];

        /* spaces */
        if (c==' '||c=='\t'||c=='\r') { l.pos++; l.col++; continue; }

        /* newline */
        if (c=='\n') {
            ll_add(&l, TK_NL, "\\n", l.line, l.col);
            l.pos++; l.line++; l.col=1; continue;
        }

        /* comment // */
        if (c=='/' && l.pos+1<l.slen && src[l.pos+1]=='/') {
            while (l.pos<l.slen && src[l.pos]!='\n') l.pos++;
            continue;
        }

        /* string "..." */
        if (c=='"') {
            int start=l.pos; l.pos++;
            while (l.pos<l.slen && src[l.pos]!='"') {
                if (src[l.pos]=='\\') l.pos++;
                l.pos++;
            }
            l.pos++; /* closing " */
            int len=l.pos-start;
            if (len>=LU_MAX_STR) len=LU_MAX_STR-1;
            memcpy(buf, src+start, len); buf[len]=0;
            ll_add(&l, TK_STR, buf, l.line, l.col);
            continue;
        }

        /* #q[n] or #q[n]:end */
        if (c=='#' && l.pos+1<l.slen && src[l.pos+1]=='q') {
            l.pos+=2;
            int qstart=l.pos;
            while (l.pos<l.slen && isdigit(src[l.pos])) l.pos++;
            int qlen=l.pos-qstart;
            if (qlen >= LU_MAX_STR) qlen = LU_MAX_STR - 1;
            memcpy(buf, src+qstart, qlen);
            buf[qlen]=0;
            int bn=atoi(buf);
            if (l.pos<l.slen && src[l.pos]==':') {
                /* :end */
                while (l.pos<l.slen && src[l.pos]!='\n') l.pos++;
                ll_add(&l, TK_BLKEND, buf, l.line, l.col);
            } else {
                LTok *t=&l.toks[l.ntok];
                ll_add(&l, TK_BLKID, buf, l.line, l.col);
                t->block=bn;
            }
            continue;
        }

        /* number */
        if (isdigit(c)) {
            int start=l.pos;
            int type=TK_INT;
            while (l.pos<l.slen && (isdigit(src[l.pos])||src[l.pos]=='.')) {
                if (src[l.pos]=='.') type=TK_FLT;
                l.pos++;
            }
            int len=l.pos-start;
            memcpy(buf,src+start,len); buf[len]=0;
            ll_add(&l, type, buf, l.line, l.col);
            continue;
        }

        /* identifier / keyword (including Lu/ keywords with /) */
        if (isalpha(c)||c=='_') {
            int start=l.pos;
            while (l.pos<l.slen) {
                char wc=src[l.pos];
                if (isalnum(wc)||wc=='_'||wc=='.') { l.pos++; continue; }
                if (wc=='/') {
                    /* include the slash for Lu/ keywords */
                    l.pos++; break;
                }
                if (wc==':') {
                    /* Def:const / Def:config */
                    int rem=l.slen-l.pos;
                    if (rem>=6 && strncmp(src+l.pos,":const",6)==0)  { l.pos+=6; break; }
                    if (rem>=7 && strncmp(src+l.pos,":config",7)==0) { l.pos+=7; break; }
                    break;
                }
                break;
            }
            int wlen=l.pos-start;
            if (wlen >= LU_MAX_STR) wlen = LU_MAX_STR - 1;
            memcpy(buf,src+start,wlen);
            buf[wlen]=0;
            int kw=ll_kw(buf,wlen);
            ll_add(&l, kw, buf, l.line, l.col);
            continue;
        }

        /* two-char operators */
        if (l.pos+1<l.slen) {
            char c2=src[l.pos+1];
            int tt=-1; const char *tv=NULL;
            if(c=='='&&c2=='='){tt=TK_EQ; tv="==";}
            else if(c=='!'&&c2=='='){tt=TK_NEQ;tv="!=";}
            else if(c=='<'&&c2=='='){tt=TK_LE; tv="<=";}
            else if(c=='>'&&c2=='='){tt=TK_GE; tv=">=";}
            else if(c=='&'&&c2=='&'){tt=TK_AND;tv="&&";}
            else if(c=='|'&&c2=='|'){tt=TK_OR; tv="||";}
            else if(c=='-'&&c2=='>'){tt=TK_DOT;tv="->";}
            if (tt>=0) { ll_add(&l,tt,tv,l.line,l.col); l.pos+=2; continue; }
        }

        /* single-char operators */
        {
            int tt=-1; char sv[3]={c,0,0};
            switch(c){
            case '+':tt=TK_PLUS; break; case '-':tt=TK_MINUS;break;
            case '*':tt=TK_STAR; break; case '/':tt=TK_SLASH; break;
            case '%':tt=TK_PCT;  break; case '<':tt=TK_LT;   break;
            case '>':tt=TK_GT;   break; case '!':tt=TK_BANG;  break;
            case '=':tt=TK_ASSIGN;break;case '(':tt=TK_LPAR; break;
            case ')':tt=TK_RPAR; break; case '{':tt=TK_LBRC; break;
            case '}':tt=TK_RBRC; break; case ',':tt=TK_COMMA;break;
            case ';':tt=TK_SEMI; break; case ':':tt=TK_COLON;break;
            case '.':tt=TK_DOT;  break; case '[':tt=TK_LBRAK;break;
            case ']':tt=TK_RBRAK;break;
            }
            if (tt>=0) { ll_add(&l,tt,sv,l.line,l.col); l.pos++; l.col++; continue; }
        }

        l.pos++; /* skip unknown */
    }
    ll_add(&l, TK_EOF, "", l.line, l.col);
    return l.ntok;
}

/* ── Type mapping ── */
static const char *lu_map_type(const char *t) {
    if (!t) return "int";
    if (strcmp(t,"int"  )==0) return "int";
    if (strcmp(t,"float")==0) return "double";
    if (strcmp(t,"str"  )==0) return "char*";
    if (strcmp(t,"bool" )==0) return "bool";
    if (strcmp(t,"void" )==0) return "void";
    if (strcmp(t,"byte" )==0) return "uint8_t";
    if (strcmp(t,"int64")==0) return "int64_t";
    return t;
}

static bool lu_is_type(int tt) {
    return tt==TK_TYINT||tt==TK_TYFLT||tt==TK_TYSTR||
           tt==TK_TYBOOL||tt==TK_TYVOID||tt==TK_TYBYTE||tt==TK_TYI64;
}

/* ── Indentation helper ── */
static void lu_indent(FILE *f, int d) {
    for(int i=0;i<d;i++) fputs("    ",f);
}

/* ── Emit tokens as expression until NL/EOF/stop_tok ── */
static int lu_emit_expr(FILE *f, LTok *toks, int ti, int ntok, int stop) {
    while (ti<ntok) {
        int tt=toks[ti].type;
        if(tt==TK_NL||tt==TK_EOF||tt==stop) break;
        /* skip To/ keyword inside expression */
        if(tt==TK_TO) { ti++; continue; }
        fprintf(f, "%s", toks[ti].val);
        ti++;
        /* add space between tokens unless next is punctuation */
        if(ti<ntok){
            int nt=toks[ti].type;
            if(nt!=TK_COMMA&&nt!=TK_RPAR&&nt!=TK_SEMI&&
               nt!=TK_NL&&nt!=TK_EOF&&nt!=stop&&nt!=TK_LBRAK&&nt!=TK_RBRAK)
                fputc(' ',f);
        }
    }
    return ti;
}

/* ── Main codegen ── */
__attribute__((noinline)) static void lu_self_codegen(FILE *ofp, LTok *toks, int ntok) {

    int ti    = 0;
    int depth = 0;
    int tmp   = 0;

    /* forward-declare all blocks */
    for(int i=0;i<ntok;i++)
        if(toks[i].type==TK_BLKID)
            fprintf(ofp,"static void _q%d(void);\n",toks[i].block);
    fprintf(ofp,"\n");

    while(ti<ntok) {
        LTok *t=&toks[ti];
        int tt=t->type;

        /* skip NL, LANG, MODE, IMPORT lines */
        if(tt==TK_NL){ti++;continue;}
        if(tt==TK_LANG||tt==TK_MODE){
            while(ti<ntok&&toks[ti].type!=TK_NL)ti++;
            continue;
        }
        if(tt==TK_IMPORT){
            while(ti<ntok&&toks[ti].type!=TK_NL)ti++;
            continue;
        }
        if(tt==TK_DEFCFG){
            while(ti<ntok&&toks[ti].type!=TK_NL)ti++;
            continue;
        }

        /* Def:const NAME = VALUE */
        if(tt==TK_DCST){
            ti++;
            const char *cname=toks[ti].val; ti++;
            ti++; /* = */
            const char *cval=toks[ti].val; ti++;
            fprintf(ofp,"#define %s %s\n",cname,cval);
            continue;
        }

        /* struct Name { ... } */
        if(tt==TK_STRUCT){
            ti++;
            const char *sname=toks[ti].val; ti++;
            fprintf(ofp,"typedef struct _%s {\n",sname);
            while(ti<ntok&&toks[ti].type!=TK_LBRC)ti++;
            ti++; /* { */
            while(ti<ntok){
                int ft=toks[ti].type;
                if(ft==TK_RBRC){ti++;break;}
                if(ft==TK_NL){ti++;continue;}
                if(lu_is_type(ft)||ft==TK_PTR){
                    const char *ftype; bool is_ptr=false;
                    if(ft==TK_PTR){is_ptr=true;ti++;ft=toks[ti].type;}
                    ftype=lu_map_type(toks[ti].val); ti++;
                    if(toks[ti].type==TK_PTR){
                        /* ptr/ptr/int etc */
                        ti++; ftype=lu_map_type(toks[ti].val); ti++;
                        is_ptr=true;
                    }
                    const char *fname2=toks[ti].val; ti++;
                    fprintf(ofp,"    %s%s %s;\n",ftype,is_ptr?"*":"",fname2);
                }else{ti++;}
            }
            fprintf(ofp,"} %s;\n\n",sname);
            continue;
        }

        /* enum Name { ... } */
        if(tt==TK_ENUM){
            ti++;
            const char *ename=toks[ti].val; ti++;
            fprintf(ofp,"typedef enum {\n");
            while(ti<ntok&&toks[ti].type!=TK_LBRC)ti++;
            ti++;
            int eval=0;
            while(ti<ntok){
                int et=toks[ti].type;
                if(et==TK_RBRC){ti++;break;}
                if(et==TK_NL||et==TK_COMMA){ti++;continue;}
                if(et==TK_IDENT){
                    fprintf(ofp,"    %s_%s = %d,\n",ename,toks[ti].val,eval++);
                    ti++;
                }else ti++;
            }
            fprintf(ofp,"} %s;\n\n",ename);
            continue;
        }

        /* Fn/name(params):ret { body } */
        if(tt==TK_FN){
            ti++;
            const char *fname=toks[ti].val; ti++;
            /* collect params */
            while(ti<ntok&&toks[ti].type!=TK_LPAR)ti++;
            ti++; /* ( */
            /* param list */
            static char psig[2048]; memset(psig,0,sizeof(psig));
            bool first_p=true;
            while(ti<ntok&&toks[ti].type!=TK_RPAR){
                int pt=toks[ti].type;
                if(pt==TK_NL||pt==TK_COMMA){ti++;continue;}
                if(lu_is_type(pt)||pt==TK_PTR){
                    bool is_ptr=false;
                    if(pt==TK_PTR){is_ptr=true;ti++;pt=toks[ti].type;}
                    const char *ptype=lu_map_type(toks[ti].val); ti++;
                    /* handle ptr/ptr/... */
                    while(ti<ntok&&toks[ti].type==TK_PTR){
                        ti++;
                        if(ti<ntok&&lu_is_type(toks[ti].type)){ti++;}
                        is_ptr=true;
                    }
                    const char *pname=toks[ti].val; ti++;
                    if(!first_p) strncat(psig,", ",sizeof(psig)-strlen(psig)-1);
                    char pbuf[128];
                    snprintf(pbuf,sizeof(pbuf),"%s%s %s",ptype,is_ptr?"*":"",pname);
                    strncat(psig,pbuf,sizeof(psig)-strlen(psig)-1);
                    first_p=false;
                }else ti++;
            }
            ti++; /* ) */
            /* :ret */
            if(ti<ntok&&toks[ti].type==TK_COLON) ti++;
            const char *rtype=lu_map_type(toks[ti].val); ti++;
            fprintf(ofp,"static %s %s(%s) {\n",rtype,fname,psig);
            depth++;
            continue;
        }

        /* #q[n] block start */
        if(tt==TK_BLKID){
            fprintf(ofp,"\nstatic void _q%d(void) {\n",t->block);
            depth=1; ti++;
            continue;
        }

        /* #q[n]:end */
        if(tt==TK_BLKEND){
            while(ti<ntok&&toks[ti].type!=TK_NL)ti++;
            if(depth>0)depth--;
            fprintf(ofp,"}\n");
            continue;
        }

        /* closing } (function body end) */
        if(tt==TK_RBRC){
            if(depth>0)depth--;
            lu_indent(ofp,depth);
            fprintf(ofp,"}\n");
            ti++; continue;
        }

        /* opening { (if/loop body) — handled inline below */

        /* Pr/ expr */
        if(tt==TK_PR){
            ti++;
            lu_indent(ofp,depth);
            int pt2=toks[ti].type;
            if(pt2==TK_STR){
                /* string literal: use puts */
                fprintf(ofp,"puts(%s);\n",toks[ti].val);
                ti++;
            }else{
                /* expression: use printf %d or generic */
                fprintf(ofp,"{ int _v%d=(",tmp++);
                ti=lu_emit_expr(ofp,toks,ti,ntok,TK_NL);
                fprintf(ofp,"); printf(\"%%d\\n\",_v%d); }\n",tmp-1);
            }
            continue;
        }

        /* Set/ lhs = expr  (lhs may include array subscript: src[n]) */
        if(tt==TK_SET){
            ti++;
            lu_indent(ofp,depth);
            /* emit LHS tokens until = sign */
            while(ti<ntok && toks[ti].type!=TK_ASSIGN && toks[ti].type!=TK_NL) {
                fprintf(ofp,"%s",toks[ti].val); ti++;
            }
            /* skip = */
            if(ti<ntok&&toks[ti].type==TK_ASSIGN)ti++;
            fprintf(ofp," = ");
            ti=lu_emit_expr(ofp,toks,ti,ntok,TK_NL);
            fprintf(ofp,";\n");
            continue;
        }

        /* Ret/ [expr] */
        if(tt==TK_RET){
            ti++;
            lu_indent(ofp,depth);
            if(toks[ti].type==TK_NL){
                fprintf(ofp,"return;\n");
            }else{
                fprintf(ofp,"return ");
                ti=lu_emit_expr(ofp,toks,ti,ntok,TK_NL);
                fprintf(ofp,";\n");
            }
            continue;
        }

        /* Break/ */
        if(tt==TK_BREAK){
            ti++;
            lu_indent(ofp,depth);
            fprintf(ofp,"break;\n");
            continue;
        }

        /* Call/ fname(args) */
        if(tt==TK_CALL){
            ti++;
            lu_indent(ofp,depth);
            /* emit until NL */
            ti=lu_emit_expr(ofp,toks,ti,ntok,TK_NL);
            fprintf(ofp,";\n");
            continue;
        }

        /* Free/ varname */
        if(tt==TK_FREE){
            ti++;
            lu_indent(ofp,depth);
            fprintf(ofp,"free(%s);\n",toks[ti].val);
            ti++;
            continue;
        }

        /* Alloc/type:count as statement  ptr/type varname = Alloc/... */
        if(tt==TK_ALLOC){
            /* skip — handled as expr below */
            ti++; while(ti<ntok&&toks[ti].type!=TK_NL)ti++;
            continue;
        }

        /* Variable declaration: type[ptr] name [= expr] */
        if(lu_is_type(tt)||tt==TK_PTR){
            bool is_ptr=false;
            if(tt==TK_PTR){is_ptr=true;ti++;}
            const char *vtype=lu_map_type(toks[ti].val); ti++;
            /* consume additional ptr/ nesting */
            while(ti<ntok&&toks[ti].type==TK_PTR){
                ti++;
                if(ti<ntok&&lu_is_type(toks[ti].type))ti++;
                is_ptr=true;
            }
            const char *vname2=toks[ti].val; ti++;
            lu_indent(ofp,depth);
            fprintf(ofp,"%s%s %s",vtype,is_ptr?"*":"",vname2);
            if(ti<ntok&&toks[ti].type==TK_ASSIGN){
                ti++;
                fprintf(ofp," = ");
                /* check for Alloc/ */
                if(ti<ntok&&toks[ti].type==TK_ALLOC){
                    ti++; /* skip Alloc/ */
                    /* type:count */
                    const char *atype="int";
                    if(ti<ntok&&lu_is_type(toks[ti].type)){
                        atype=lu_map_type(toks[ti].val); ti++;
                    }
                    /* skip : */
                    if(ti<ntok&&toks[ti].type==TK_COLON)ti++;
                    fprintf(ofp,"(%s*)malloc(sizeof(%s)*(", atype, atype);
                    ti=lu_emit_expr(ofp,toks,ti,ntok,TK_NL);
                    fprintf(ofp,"))");
                }else{
                    ti=lu_emit_expr(ofp,toks,ti,ntok,TK_NL);
                }
            }
            fprintf(ofp,";\n");
            continue;
        }

        /* If/ condition { body } [Elif/ ...] [Else/ { body }] */
        if(tt==TK_IF||tt==TK_ELIF){
            ti++;
            lu_indent(ofp,depth);
            fprintf(ofp,tt==TK_IF?"if (":"else if (");
            /* condition up to { or To/ */
            while(ti<ntok){
                int ct=toks[ti].type;
                if(ct==TK_LBRC||ct==TK_TO)break;
                if(ct==TK_NL)break;
                fprintf(ofp,"%s ",toks[ti].val);
                ti++;
            }
            fprintf(ofp,") ");
            if(ti<ntok&&toks[ti].type==TK_LBRC){
                fprintf(ofp,"{\n"); ti++; depth++;
            }else if(ti<ntok&&toks[ti].type==TK_TO){
                ti++;
                fprintf(ofp,"{\n"); depth++;
            }else{
                fprintf(ofp,"{\n"); depth++;
            }
            continue;
        }

        /* Else/ */
        if(tt==TK_ELSE){
            ti++;
            /* close previous if block if needed */
            if(depth>0){depth--; lu_indent(ofp,depth); fprintf(ofp,"} ");}
            fprintf(ofp,"else ");
            if(ti<ntok&&toks[ti].type==TK_LBRC){
                fprintf(ofp,"{\n"); ti++; depth++;
            }else{
                fprintf(ofp,"{\n"); depth++;
            }
            continue;
        }

        /* To/ — inline then-branch (already handled in If/ above) */
        if(tt==TK_TO){ti++;continue;}

        /* Loop/While cond { body } or Loop/N { body } */
        if(tt==TK_LOOP){
            ti++;
            lu_indent(ofp,depth);
            /* "While" or integer count */
            if(ti<ntok&&strcmp(toks[ti].val,"While")==0){
                ti++;
                fprintf(ofp,"while (");
                while(ti<ntok){
                    int ct=toks[ti].type;
                    if(ct==TK_LBRC||ct==TK_NL)break;
                    fprintf(ofp,"%s ",toks[ti].val);
                    ti++;
                }
                fprintf(ofp,") ");
            }else{
                /* Loop/N — repeat N times */
                char cvar[32]; snprintf(cvar,sizeof(cvar),"_li%d",tmp++);
                const char *cnt_val=toks[ti].val; ti++;
                fprintf(ofp,"for(int %s=0;%s<%s;%s++) ",cvar,cvar,cnt_val,cvar);
            }
            if(ti<ntok&&toks[ti].type==TK_LBRC){
                fprintf(ofp,"{\n"); ti++; depth++;
            }else{
                fprintf(ofp,"{\n"); depth++;
            }
            continue;
        }

        /* Try/ */
        if(tt==TK_SET+30/* placeholder */){
            /* skip for now */ ti++;continue;
        }

        /* generic IDENT statement — function call or expression */
        if(tt==TK_IDENT){
            /* check if next token is ( → function call */
            if(ti+1<ntok&&toks[ti+1].type==TK_LPAR){
                lu_indent(ofp,depth);
                ti=lu_emit_expr(ofp,toks,ti,ntok,TK_NL);
                fprintf(ofp,";\n");
                continue;
            }
            /* otherwise skip */
            ti++; continue;
        }

        /* EOF */
        if(tt==TK_EOF)break;

        ti++;
    }
}


/* ── Cast helpers for Lu self-hosting (lu_compiler.lu calls these) ── */
#define lu_self_lex_cast(src,n,toks)      lu_self_lex((char*)(src),(n),(LTok*)(toks))
/* size check: LTok must match allocation */
#define lu_self_codegen_cast(ofp,toks,n)  lu_self_codegen((FILE*)(ofp),(LTok*)(toks),(n))



/* ── Safe FILE* wrappers for Lu self-hosting ── */
static inline char *luc_fopen(const char *p, const char *m) { return (char*)fopen(p,m); }
static inline void  luc_fclose(char *f)                     { if(f) fclose((FILE*)f); }
static inline int   luc_fread(char *buf,int sz,int n,char *f){ return (int)fread(buf,sz,n,(FILE*)f); }
static inline void  luc_fprintf0(char *f, const char *s)    { fprintf((FILE*)f,"%s",s); }
#define LUC_FPRINTF(f,...) fprintf((FILE*)(f),__VA_ARGS__)

#endif /* LU_SELF_RUNTIME_H */
