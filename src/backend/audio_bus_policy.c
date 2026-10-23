#include "audio_bus_policy.h"
#include <stdlib.h>
#include <string.h>

typedef struct bp_row {
    uint32_t bus;
    sh_audio_bus_record field;
    const unsigned char *body;
    const sh_audio_graph_root *root;
    size_t recipe,value;
    int removed,edited,baseline;
} bp_row;
typedef struct bp_rows { bp_row *items;size_t count,allocated; } bp_rows;
typedef struct bp_group { size_t first,end,winner;int empty_mode; } bp_group;
typedef struct bp_bus { uint32_t id;unsigned kind;size_t parent;int baseline; } bp_bus;
static int bp_fail(char *error,size_t capacity,const char *message)
{ if(error && capacity)snprintf(error,capacity,"%s",message);return 0; }
static uint32_t bp_u32(const unsigned char *p)
{ return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static int bp_push(bp_rows *rows,bp_row row)
{
    if(rows->count==rows->allocated) {
        size_t next=rows->allocated ? rows->allocated*2 : 32;
        if(next<rows->allocated || next>SIZE_MAX/sizeof(*rows->items))return 0;
        bp_row *items=realloc(rows->items,next*sizeof(*items));if(!items)return 0;
        rows->items=items;rows->allocated=next;
    }
    rows->items[rows->count++]=row;return 1;
}
static int bp_key(const bp_row *a,const bp_row *b)
{
    if(a->bus!=b->bus)return a->bus<b->bus ? -1 : 1;
    if(a->field.field!=b->field.field)return a->field.field<b->field.field ? -1 : 1;
    for(size_t i=0;i<2;i++)if(a->field.key[i]!=b->field.key[i])return a->field.key[i]<b->field.key[i] ? -1 : 1;
    return 0;
}
static int bp_bytes(const bp_row *a,const bp_row *b)
{
    size_t length=a->field.length<b->field.length ? a->field.length : b->field.length;
    int order=length ? memcmp(a->body,b->body,length) : 0;
    return order ? order : a->field.length<b->field.length ? -1 : a->field.length>b->field.length;
}
static int bp_same(const sh_audio_link_graph *graph,const bp_row *a,const bp_row *b)
{
    if(a->removed || b->removed)return a->removed && b->removed;
    return !bp_bytes(a,b) && sh_audio_link_graph_equal(graph,a->root->node,b->root->node);
}
static int bp_rank(const bp_row *a,const bp_row *b)
{
    if(a->edited!=b->edited)return a->edited ? -1 : 1;
    if(a->removed!=b->removed)return a->removed ? 1 : -1;
    if(!a->edited && a->baseline!=b->baseline)return a->baseline ? -1 : 1;
    int order=a->removed ? 0 : bp_bytes(a,b);
    return order ? order : memcmp(a->root->identity,b->root->identity,32);
}
static int bp_order(const void *left,const void *right)
{ const bp_row *a=left,*b=right;int order=bp_key(a,b);return order ? order : bp_rank(a,b); }
static size_t bp_local(const bp_rows *rows,size_t first,const bp_row *key)
{ for(size_t i=first;i<rows->count;i++)if(!bp_key(rows->items+i,key))return i;return SIZE_MAX; }
static int bp_assign(bp_rows *rows,size_t first,bp_row row,int keep_first)
{
    size_t at=bp_local(rows,first,&row);
    if(at==SIZE_MAX)return bp_push(rows,row);
    if(!keep_first)rows->items[at]=row;return 1;
}
static size_t bp_curves(const bp_rows *rows,size_t first,uint32_t parameter)
{
    size_t count=0;
    for(size_t i=first;i<rows->count;i++)count+=rows->items[i].field.field==SH_AUDIO_BUS_FIELD_RTPC_CURVE &&
        rows->items[i].field.key[0]==parameter && !rows->items[i].removed;
    return count;
}
static int bp_recipe(bp_rows *rows,const sh_audio_bus_recipe_input *input,size_t recipe)
{
    size_t first=rows->count;const sh_audio_bus_value_input *mode=NULL;
    for(size_t i=0;i<input->count;i++) {
        const sh_audio_bus_value_input *value=input->values+i;
        bp_row row={input->bus,value->field,value->body,&value->root,recipe,value->origin,0,0,input->baseline};
        if(row.field.field==SH_AUDIO_BUS_FIELD_RTPC_MODE){mode=value;continue;}
        if(row.field.field==SH_AUDIO_BUS_FIELD_RTPC_CURVE) {
            uint32_t parameter=row.field.key[0];
            if(!mode || mode->field.key[0]!=parameter || row.field.length<14)return 0;
            size_t previous=bp_curves(rows,first,parameter);
            row.removed=!row.body[12] && !row.body[13];
            if(!bp_assign(rows,first,row,0))return 0;
            bp_row control={input->bus,mode->field,mode->body,&mode->root,recipe,mode->origin,0,0,input->baseline};
            /* The native subscription retains its first mode while it has
             * curves. Removing its final curve destroys that subscription. */
            if(!bp_curves(rows,first,parameter)) {
                control.removed=1;if(!bp_assign(rows,first,control,0))return 0;
            } else if(!previous && !bp_assign(rows,first,control,0))return 0;
            mode=NULL;continue;
        }
        if(!bp_assign(rows,first,row,row.field.field==SH_AUDIO_BUS_FIELD_PROPERTY))return 0;
    }
    return mode==NULL;
}
static int bp_collection(sh_audio_bus_field field)
{
    return field==SH_AUDIO_BUS_FIELD_PROPERTY || field==SH_AUDIO_BUS_FIELD_DUCK ||
        field==SH_AUDIO_BUS_FIELD_BYPASS || field==SH_AUDIO_BUS_FIELD_EFFECT ||
        field==SH_AUDIO_BUS_FIELD_RTPC_MODE || field==SH_AUDIO_BUS_FIELD_RTPC_CURVE ||
        field==SH_AUDIO_BUS_FIELD_STATE_GROUP || field==SH_AUDIO_BUS_FIELD_STATE;
}
static size_t bp_bus_find(const bp_bus *buses,size_t count,uint32_t id)
{
    size_t first=0,last=count;
    while(first<last){size_t mid=first+(last-first)/2;if(buses[mid].id<id)first=mid+1;else last=mid;}
    return first<count && buses[first].id==id ? first : SIZE_MAX;
}
static int bp_bus_order(const void *left,const void *right)
{ uint32_t a=((const bp_bus *)left)->id,b=((const bp_bus *)right)->id;return a<b ? -1 : a>b; }
static size_t bp_group_find(const bp_rows *rows,const bp_group *groups,size_t count,
    uint32_t bus,sh_audio_bus_field field,uint32_t key)
{
    for(size_t i=0;i<count;i++) {
        const bp_row *row=rows->items+groups[i].first;
        if(row->bus==bus && row->field.field==field && row->field.key[0]==key)return i;
    }
    return SIZE_MAX;
}
static int bp_parent_allowed(const bp_rows *rows,const bp_group *groups,const bp_bus *buses,
    size_t count,size_t owner,size_t candidate)
{
    if(rows->items[candidate].removed || rows->items[candidate].field.length!=4)return 0;
    uint32_t parent=bp_u32(rows->items[candidate].body);
    for(size_t steps=0;parent && steps<=count;steps++) {
        size_t target=bp_bus_find(buses,count,parent);
        if(target==SIZE_MAX)return 1;
        if(target==owner)return 0;
        parent=bp_u32(rows->items[groups[buses[target].parent].winner].body);
    }
    return !parent;
}
static int bp_parents(const bp_rows *rows,bp_group *groups,bp_bus *buses,size_t count)
{
    unsigned char *ready=calloc(count ? count : 1,1);if(!ready)return 0;
    for(size_t used=0;used<count;used++) {
        size_t chosen=SIZE_MAX,candidate=SIZE_MAX;
        for(size_t b=0;b<count;b++)if(!ready[b]) {
            bp_group *group=groups+buses[b].parent;
            for(size_t r=group->first;r<group->end;r++) {
                const bp_row *row=rows->items+r;
                if(row->removed || row->field.length!=4)continue;
                uint32_t parent=bp_u32(row->body);size_t target=bp_bus_find(buses,count,parent);
                if(parent && target!=SIZE_MAX && !ready[target])continue;
                if(candidate==SIZE_MAX || bp_rank(row,rows->items+candidate)<0 ||
                    (!bp_rank(row,rows->items+candidate) && buses[b].id<buses[chosen].id)){chosen=b;candidate=r;}
                break;
            }
        }
        if(chosen==SIZE_MAX){free(ready);return 0;}
        groups[buses[chosen].parent].winner=candidate;ready[chosen]=1;
    }
    free(ready);
    /* Improve the rooted forest monotonically. A candidate is accepted only
     * when it cannot introduce a cycle, independent of package enumeration. */
    for(int changed=1;changed;) {
        changed=0;
        for(size_t b=0;b<count;b++) {
            bp_group *group=groups+buses[b].parent;
            for(size_t r=group->first;r<group->winner;r++)
                if(bp_parent_allowed(rows,groups,buses,count,b,r)) {group->winner=r;changed=1;break;}
        }
    }
    return 1;
}
static int bp_require_mode(const bp_rows *rows,bp_group *groups,size_t count,
    uint32_t bus,sh_audio_bus_field field,uint32_t key)
{
    size_t g=bp_group_find(rows,groups,count,bus,field,key);if(g==SIZE_MAX)return 0;
    if(!rows->items[groups[g].winner].removed)return 1;
    for(size_t r=groups[g].first;r<groups[g].end;r++)if(!rows->items[r].removed){groups[g].winner=r;return 1;}
    return 0;
}
void sh_audio_bus_policy_free(sh_audio_bus_policy *policy)
{
    if(!policy)return;
    for(size_t i=0;i<policy->count;i++){free(policy->settings[i].body);free(policy->settings[i].contributors);}
    free(policy->settings);free(policy->buses);memset(policy,0,sizeof(*policy));
}
int sh_audio_bus_policy_compile(const sh_audio_link_graph *graph,const sh_audio_bus_recipe_input *inputs,
    size_t count,sh_audio_bus_policy *out,char *error,size_t capacity)
{
    bp_rows local={0},rows={0};bp_group *groups=NULL;bp_bus *buses=NULL;
    size_t *starts=NULL,*baseline=NULL,group_count=0,bus_count=0;sh_audio_bus_policy policy={0};int ok=0;
    if(error && capacity)*error=0;if(!out)return 0;memset(out,0,sizeof(*out));
    if(!graph || (count && !inputs) || count>=SIZE_MAX/sizeof(*starts) || count>SIZE_MAX/sizeof(*buses))goto invalid;
    starts=calloc(count+1,sizeof(*starts));baseline=malloc((count ? count : 1)*sizeof(*baseline));
    buses=calloc(count ? count : 1,sizeof(*buses));if(!starts || !baseline || !buses)goto memory;
    for(size_t i=0;i<count;i++) {
        const sh_audio_bus_recipe_input *input=inputs+i;
        if(!input->bus || (input->kind!=8 && input->kind!=20) || !input->count || !input->values ||
            !sh_audio_link_graph_equal(graph,input->root.node,input->root.node))goto invalid;
        baseline[i]=SIZE_MAX;
        for(size_t j=0;j<count;j++)if(inputs[j].baseline && inputs[j].bus==input->bus){baseline[i]=j;break;}
        starts[i]=local.count;
        for(size_t v=0;v<input->count;v++)if(!input->values[v].body || !input->values[v].field.length ||
            input->values[v].field.field>SH_AUDIO_BUS_FIELD_STATE ||
            !sh_audio_link_graph_equal(graph,input->values[v].root.node,input->values[v].root.node))goto invalid;
        if(!bp_recipe(&local,input,i))goto invalid;
        size_t b=0;while(b<bus_count && buses[b].id!=input->bus)b++;
        if(b==bus_count)buses[bus_count++]=(bp_bus){input->bus,input->kind,SIZE_MAX,input->baseline};
        else if((input->baseline && !buses[b].baseline) ||
            (!input->baseline && !buses[b].baseline && input->kind<buses[b].kind)) {
            buses[b].kind=input->kind;buses[b].baseline=input->baseline;
        }
    }
    starts[count]=local.count;
    for(size_t i=0;i<count;i++) {
        const sh_audio_bus_recipe_input *input=inputs+i;size_t base=baseline[i];
        if((input->baseline && base!=i) || (!input->baseline && input->root.original && base!=SIZE_MAX &&
            !sh_audio_link_graph_equal(graph,input->root.node,inputs[base].root.node)))continue;
        for(size_t r=starts[i];r<starts[i+1];r++) {
            bp_row row=local.items[r];const bp_row *original=NULL;
            if(base!=SIZE_MAX)for(size_t b=starts[base];b<starts[base+1];b++)if(!bp_key(&row,local.items+b)){original=local.items+b;break;}
            row.edited=!input->baseline && (original ? !bp_same(graph,&row,original) : !row.removed);
            if(!bp_push(&rows,row))goto memory;
        }
        if(!input->baseline && base!=SIZE_MAX)for(size_t b=starts[base];b<starts[base+1];b++) {
            const bp_row *original=local.items+b;if(original->removed || !bp_collection(original->field.field))continue;
            size_t r=starts[i];while(r<starts[i+1] && bp_key(original,local.items+r))r++;
            if(r==starts[i+1]) {
                bp_row removed=*original;removed.recipe=i;removed.value=SIZE_MAX;removed.removed=removed.edited=1;
                removed.baseline=0;removed.root=&input->root;removed.body=NULL;removed.field.length=0;
                if(!bp_push(&rows,removed))goto memory;
            }
        }
    }
    if(rows.count>1)qsort(rows.items,rows.count,sizeof(*rows.items),bp_order);
    groups=calloc(rows.count ? rows.count : 1,sizeof(*groups));if(!groups)goto memory;
    for(size_t first=0;first<rows.count;) {
        size_t end=first+1;while(end<rows.count && !bp_key(rows.items+first,rows.items+end))end++;
        for(size_t r=first+1;r<end;r++)if(!rows.items[r].removed && !rows.items[r-1].removed &&
            !bp_bytes(rows.items+r,rows.items+r-1) && !memcmp(rows.items[r].root->identity,rows.items[r-1].root->identity,32) &&
            !bp_same(graph,rows.items+r,rows.items+r-1)) {
            bp_fail(error,capacity,"audio bus fingerprint matched different dependency bindings");goto done;
        }
        groups[group_count++]=(bp_group){first,end,first};first=end;
    }
    qsort(buses,bus_count,sizeof(*buses),bp_bus_order);
    for(size_t b=0;b<bus_count;b++) {
        buses[b].parent=bp_group_find(&rows,groups,group_count,buses[b].id,SH_AUDIO_BUS_FIELD_PARENT,0);
        if(buses[b].parent==SIZE_MAX)goto invalid;
    }
    if(!bp_parents(&rows,groups,buses,bus_count)) {
        bp_fail(error,capacity,"audio bus parents have no rooted combination of authored or native choices");goto done;
    }
    for(size_t g=0;g<group_count;g++) {
        const bp_row *row=rows.items+groups[g].winner;if(row->removed)continue;
        if(row->field.field==SH_AUDIO_BUS_FIELD_RTPC_CURVE && !bp_require_mode(&rows,groups,group_count,row->bus,
            SH_AUDIO_BUS_FIELD_RTPC_MODE,row->field.key[0]))goto invalid;
        if(row->field.field==SH_AUDIO_BUS_FIELD_STATE && !bp_require_mode(&rows,groups,group_count,row->bus,
            SH_AUDIO_BUS_FIELD_STATE_GROUP,row->field.key[0]))goto invalid;
    }
    for(size_t g=0;g<group_count;g++) {
        const bp_row *row=rows.items+groups[g].winner;
        if(row->field.field!=SH_AUDIO_BUS_FIELD_RTPC_MODE)continue;
        int curves=0;
        for(size_t c=0;c<group_count;c++) {
            const bp_row *curve=rows.items+groups[c].winner;
            curves|=curve->bus==row->bus && curve->field.field==SH_AUDIO_BUS_FIELD_RTPC_CURVE &&
                curve->field.key[0]==row->field.key[0] && !curve->removed;
        }
        groups[g].empty_mode=!curves;
    }
    policy.buses=calloc(bus_count ? bus_count : 1,sizeof(*policy.buses));
    policy.settings=calloc(group_count ? group_count : 1,sizeof(*policy.settings));
    if(!policy.buses || !policy.settings)goto memory;
    for(size_t b=0;b<bus_count;b++)policy.buses[policy.bus_count++]=(sh_audio_bus_type){buses[b].id,buses[b].kind};
    for(size_t g=0;g<group_count;g++) {
        const bp_group *group=groups+g;const bp_row *winner=rows.items+group->winner;
        sh_audio_bus_setting *setting=policy.settings+policy.count++;
        setting->bus=winner->bus;setting->field=winner->field;setting->field.offset=0;
        setting->removed=winner->removed || group->empty_mode;setting->recipe=winner->recipe;
        /* A mode left without curves exists only because edits removed them. */
        setting->edited=winner->edited || group->empty_mode;
        setting->value=group->empty_mode ? SIZE_MAX : winner->value;
        if(setting->removed)setting->field.length=0;
        else {
            setting->body=malloc(winner->field.length);if(!setting->body)goto memory;
            memcpy(setting->body,winner->body,winner->field.length);
        }
        size_t maximum=group->empty_mode ? rows.count : group->end-group->first;
        if(maximum>SIZE_MAX/sizeof(*setting->contributors))goto memory;
        setting->contributors=malloc(maximum*sizeof(*setting->contributors));if(!setting->contributors)goto memory;
        if(group->empty_mode) {
            for(size_t c=0;c<group_count;c++) {
                const bp_row *curve=rows.items+groups[c].winner;
                if(curve->bus!=winner->bus || curve->field.field!=SH_AUDIO_BUS_FIELD_RTPC_CURVE ||
                    curve->field.key[0]!=winner->field.key[0])continue;
                for(size_t r=groups[c].first;r<groups[c].end;r++)if(bp_same(graph,curve,rows.items+r))
                    setting->contributors[setting->contributor_count++]=(sh_audio_bus_contributor){rows.items[r].recipe,rows.items[r].value};
            }
        } else for(size_t r=group->first;r<group->end;r++)if(bp_same(graph,winner,rows.items+r))
            setting->contributors[setting->contributor_count++]=(sh_audio_bus_contributor){rows.items[r].recipe,rows.items[r].value};
    }
    *out=policy;memset(&policy,0,sizeof(policy));ok=1;goto done;
invalid: bp_fail(error,capacity,"audio bus composition needs complete checked recipes");goto done;
memory: bp_fail(error,capacity,"cannot allocate audio bus composition");
done:
    free(local.items);free(rows.items);free(groups);free(buses);free(starts);free(baseline);
    sh_audio_bus_policy_free(&policy);return ok;
}
