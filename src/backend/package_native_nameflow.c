#include "package_native_nameflow.h"

#include <string.h>
#include <stdlib.h>
#include <limits.h>

sh_native_nameflow_kind sh_native_nameflow_member(const sh_decl_dependency_schema *schema,
    void *context, int (*derives)(void *context, const char *class_name, const char *base_name),
    sh_decl_value_type type, const sh_decl_reference_site *site)
{
    sh_decl_value_type member = {0};
    sh_decl_member_storage storage = {0};
    sh_decl_value_shape shape = {0};
    int trigger, count;
    long long offset;
    if (!site || !site->owner_type || !site->member || strcmp(site->member, "call"))
        return SH_NATIVE_NAMEFLOW_NONE;
    if (!derives) return SH_NATIVE_NAMEFLOW_UNAVAILABLE;
    trigger = derives(context, site->owner_type, "idTrigger");
    count = derives(context, site->owner_type, "idTarget_Count");
    if (trigger != 1 && count != 1)
        return trigger < 0 || count < 0 ? SH_NATIVE_NAMEFLOW_UNAVAILABLE : SH_NATIVE_NAMEFLOW_NONE;
    /* Actual paired callers read idStr.length/data at ca8/cb0 and c98/ca0.
     * A derived class may shadow a spelling, so verify the selected storage. */
    offset = trigger == 1 ? 0xca0 : 0xc90;
    if (!schema || !schema->field || !schema->describe || !schema->storage ||
        !type.name || strcmp(type.name, "idStr") || (type.ops && *type.ops) ||
        schema->field(schema->context, (sh_decl_value_type){site->owner_type, ""}, "call", &member) != 1 ||
        !member.name || strcmp(member.name, type.name) || (member.ops && *member.ops) ||
        !schema->describe(schema->context, type, &shape) || shape.kind != SH_DECL_VALUE_IGNORE ||
        schema->storage(schema->context, (sh_decl_value_type){site->owner_type, ""}, "call", &storage) != 1 ||
        storage.offset != offset || storage.extent != 0x30 || storage.bit_count || storage.bit_first)
        return SH_NATIVE_NAMEFLOW_UNAVAILABLE;
    return SH_NATIVE_NAMEFLOW_SCRIPT_CALL;
}

typedef struct nf_record {
    const char *record,*member,*nested,*string,*selector;
    long long offset,extent,selector_offset;
    int32_t enabled;
} nf_record;
static const nf_record nf_records[]={
    {"narrativeAction_t","scriptParms","narrativeAction_t::narrativeScriptParms_t","idStr","narrativeActionType_t",0x80,0x30,0,10},
    {"idNarrativeLinkCondition","scriptParms","idNarrativeLinkCondition::narrativeScriptParms_t","idStr","narrativeConditionType_t",0x10,0x30,4,14},
    {"splineMoverModifier_t","Script","splineMoverModifier_t::splineMoverModifierScript_t","idAtomicString",NULL,0xc0,0x10,0,0}
};
static int nf_type(sh_decl_value_type a,const char *name)
{return a.name && !strcmp(a.name,name) && (!a.ops || !*a.ops);}
static int nf_storage(const sh_decl_dependency_schema *s,const char *owner,const char *key,long long offset,long long extent)
{
    sh_decl_member_storage store={0};
    return s->storage && s->storage(s->context,(sh_decl_value_type){owner,""},key,&store)==1 &&
        store.offset==offset && store.extent==extent && !store.bit_count && !store.bit_first;
}
static const sh_decl_node *nf_last(const sh_decl_node *parent,const char *key,size_t length)
{
    const sh_decl_node *found=NULL;
    for(const sh_decl_node *n=parent?parent->children:NULL;n;n=n->next)
        if(n->key && strlen(n->key)==length && !memcmp(n->key,key,length))found=n;
    return found;
}
/* Resolve only the actual typed path in a prepared state. Collection limits
 * are native signed counts; no bounded prefix or guessed sparse row is used. */
static int nf_locate(const sh_decl_dependency_schema *s,const sh_native_nameflow_scope *scope,
    const char *path,const sh_decl_node **record,sh_decl_value_type *record_type,const sh_decl_node **leaf)
{
    const sh_decl_node *at=scope->state;sh_decl_value_type type=scope->type;
    if(!at || !path || strncmp(path,"edit",4) || (path[4] && path[4]!='.'))return -1;
    path+=4;if(*path=='.')path++;
    const sh_decl_node *parent=NULL;sh_decl_value_type parent_type={0};
    while(*path){
        const char *end=strchr(path,'.');size_t n=end?(size_t)(end-path):strlen(path);
        sh_decl_value_shape shape={0};sh_decl_value_type selected={0};
        if(!n || !type.name || !s->describe(s->context,type,&shape))return -1;
        char *key=malloc(n+1);if(!key)return -1;memcpy(key,path,n);key[n]=0;
        if(shape.kind==SH_DECL_VALUE_OBJECT){
            if(s->field(s->context,type,key,&selected)!=1){free(key);return -1;}
        }else if(shape.kind==SH_DECL_VALUE_COLLECTION){
            const char *prefix=shape.item_key;size_t index=0,count=shape.count;int bounded=shape.count_known;
            size_t p=prefix?strcspn(prefix,"["):0;
            if(!prefix || n<=p+2 || memcmp(path,prefix,p) || path[p]!='[' || path[n-1]!=']'){free(key);return -1;}
            for(size_t i=p+1;i<n-1;i++){unsigned d=(unsigned char)path[i]-'0';if(d>9 || index>(SIZE_MAX-d)/10){free(key);return -1;}index=index*10+d;}
            if(shape.count_key){const sh_decl_node *control=nf_last(at,shape.count_key,strlen(shape.count_key));
                if(control){if(!sh_decl_tree_list_count(control,&count)){free(key);return -1;}bounded=1;}}
            if(bounded && index>=count){free(key);return 0;}
            selected=shape.element;
        }else{free(key);return -1;}
        const sh_decl_node *next=nf_last(at,key,n);free(key);if(!next)return -1;
        if(!end){*record=parent;*record_type=parent_type;*leaf=next;return 1;}
        parent=at;parent_type=type;at=next;type=selected;path=end+1;
    }return -1;
}
sh_native_nameflow_kind sh_native_nameflow_scoped_member(const sh_decl_dependency_schema *s,
    void *context,int (*derives)(void *,const char *,const char *),sh_decl_value_type type,
    const sh_decl_reference_site *site,const char *path,const sh_native_nameflow_scope *scope,
    const char **value,size_t *length)
{
    if(value)*value=NULL;if(length)*length=0;
    sh_native_nameflow_kind simple=sh_native_nameflow_member(s,context,derives,type,site);
    if(simple!=SH_NATIVE_NAMEFLOW_NONE)return simple;
    if(!s || !s->field || !s->describe || !site || !site->owner_type || !site->member ||
       strcmp(site->member,"scriptFunction"))return SH_NATIVE_NAMEFLOW_NONE;
    const nf_record *contract=NULL;
    for(size_t i=0;i<sizeof nf_records/sizeof *nf_records;i++){
        sh_decl_value_type nested={0};const nf_record *r=nf_records+i;
        if(!strcmp(site->owner_type,r->nested)){contract=r;break;}
        if(s->field(s->context,(sh_decl_value_type){r->record,""},r->member,&nested)==1 &&
           nested.name && !strcmp(nested.name,site->owner_type)){contract=r;break;}
    }
    if(!contract)return SH_NATIVE_NAMEFLOW_NONE;
    sh_decl_value_type nested={0},field={0};sh_decl_value_shape shape={0};
    if(!value || !length || !scope || !nf_type(type,contract->string) ||
       s->field(s->context,(sh_decl_value_type){contract->record,""},contract->member,&nested)!=1 ||
       !nf_type(nested,site->owner_type) || !nf_storage(s,contract->record,contract->member,contract->offset,contract->extent) ||
       s->field(s->context,nested,"scriptFunction",&field)!=1 || !nf_type(field,contract->string) ||
       !nf_storage(s,site->owner_type,"scriptFunction",0,!strcmp(contract->string,"idStr")?0x30:8) ||
       !s->describe(s->context,type,&shape) || shape.kind!=SH_DECL_VALUE_IGNORE)return SH_NATIVE_NAMEFLOW_UNAVAILABLE;
    const sh_decl_node *record=NULL,*leaf=NULL;sh_decl_value_type record_type={0};
    int located=nf_locate(s,scope,path,&record,&record_type,&leaf);
    if(!located)return SH_NATIVE_NAMEFLOW_INACTIVE;
    if(located<0 || !nf_type(record_type,contract->record))return SH_NATIVE_NAMEFLOW_UNAVAILABLE;
    if(!sh_decl_tree_literal(leaf,value,length))return SH_NATIVE_NAMEFLOW_UNAVAILABLE;
    /* Every selected script route has the same no-dot behavior, including an
     * unestablished selector: local functions perform no entity lookup. */
    if(!memchr(*value,'.',*length))return SH_NATIVE_NAMEFLOW_INACTIVE;
    if(contract->selector){
        sh_decl_value_type selector={0};int32_t actual=0;
        const sh_decl_node *token=nf_last(record,"type",4);
        if(!token || !scope->enum_value ||
           s->field(s->context,record_type,"type",&selector)!=1 || !nf_type(selector,contract->selector) ||
           !nf_storage(s,contract->record,"type",contract->selector_offset,4) ||
           !scope->enum_value(scope->context,selector,token,&actual))return SH_NATIVE_NAMEFLOW_UNAVAILABLE;
        if(actual!=contract->enabled)return SH_NATIVE_NAMEFLOW_INACTIVE;
    }
    return SH_NATIVE_NAMEFLOW_SCRIPT_CALL;
}

int sh_native_nameflow_key(sh_native_nameflow_kind kind, const char *value, size_t length,
    const char **key, size_t *key_length)
{
    const char *dot;
    if (key) *key = NULL;
    if (key_length) *key_length = 0;
    if (kind == SH_NATIVE_NAMEFLOW_NONE) return 0;
    if (kind != SH_NATIVE_NAMEFLOW_SCRIPT_CALL || !value || !key || !key_length ||
        memchr(value, 0, length)) return -1;
    if (!length || !(dot = memchr(value, '.', length))) return 0;
    *key = value;
    *key_length = (size_t)(dot - value);
    return 1;
}
