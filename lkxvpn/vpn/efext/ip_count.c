#include <efext.h>
#include <efnet.h>
#include <efio.h>
#include <pthread.h>
#include <stdio.h>

#define MAX_COUNT_BUF       1024
#define IP_EACH_BUF_SIZE    1000000
#define IP_HASH_SIZE        0x1000000

#define TOP_TYPE            8
#define TOP_PPS_IN          0
#define TOP_PPS_OUT         1
#define TOP_BPS_IN          2
#define TOP_BPS_OUT         3
#define TOP_NEW_SESSION     4
#define TOP_NEW_HTTP        5
#define TOP_ICMP_BPS        6
#define TOP_HTTP_BPS        7
#define TOP_N               10

#define LIMIT_TYPE          4
#define LIMIT_PPS_IN        0
#define LIMIT_PPS_OUT       1
#define LIMIT_BPS_IN        2
#define LIMIT_BPS_OUT       3

#define MAX_NORMAL_VALUE        2
#define NORMAL_VALUE_FLOW       0
#define NORMAL_VALUE_SESSION    1

typedef struct _normal_value
{
    unsigned long total;
    unsigned long value;
    unsigned long time, ab_time;
}normal_value;


typedef struct _ip_info
{
    unsigned int ip;
    unsigned long attack, attack_max_pps, attack_max_bps;
    unsigned long recv, send, inflow, outflow, pps_in, pps_out, bps_in, bps_out;
    unsigned long tcp_flow, udp_flow, icmp_flow, http_flow;
    unsigned long session_total, session_close, session_timeout, http_session;
    unsigned long last_recv, last_send, last_inflow, last_outflow, last_time;
    unsigned long last_session_total, last_http_session, last_tcp_flow, last_udp_flow, last_icmp_flow, last_http_flow;
    unsigned long new_session, new_http_session, tcp_bps, udp_bps, icmp_bps, http_bps;
    normal_value normal[MAX_NORMAL_VALUE];
    struct _ip_info *prev, *next, *prev_use, *next_use, *next_alive;
}ip_info;

typedef struct _top_info
{
    unsigned int ip;
    unsigned long val;
}top_info;

struct _ip_count_t
{
    ip_info *buf[MAX_COUNT_BUF];
    ip_info *cur, *alive, *use_head, *use_tail;
    ip_info **table;
    top_info top[TOP_TYPE][TOP_N + 1];
    unsigned long limit[LIMIT_TYPE];
    volatile unsigned int stats;
    unsigned int buf_total, use_total, max_level;
    unsigned char lock, add_lock, del_lock, top_lock;
    unsigned char *table_lock;
    unsigned long time;
    void *attack_cbk;
    pthread_t counter;
    pthread_t timer;
};

int max_level = 0;

extern unsigned long base_time;



static void lock(char *lock)
{
    while(__sync_lock_test_and_set(lock, 1));
}

static void trylock(char *lock)
{
    return !(__sync_lock_test_and_set(lock, 1));
}

static void unlock(char *lock)
{
    __sync_lock_test_and_set(lock, 0);
}

int ipcount_lock(ip_count_t *ict)
{
    if(!ict)
        return 0;
    while(__sync_lock_test_and_set(&(ict->lock), 1));
}

int ipcount_unlock(ip_count_t *ict)
{
    if(!ict)
        return 0;
    __sync_lock_test_and_set(&(ict->lock), 0);
}

static void count_timer(void *arg)
{
    ip_count_t *ict = (ip_count_t *)arg;
    struct timeval now;
	while(ict->stats)
	{
		gettimeofday(&now, NULL);
		ict->time = now.tv_sec * 1000000 + now.tv_usec;
		usleep(0);
	}
}

static void top_repeat(top_info *top)
{
    int pos = 1, t1 = 0, t2 = 0, cmp = 0;
    unsigned int ip;
    unsigned long val;

    while(1)
    {
        t1 = 2 * pos; ///左孩子(存在的话)
        t2 = t1 + 1;    ///右孩子(存在的话)
        if(t1 > TOP_N)    ///无孩子节点
            break;
        else
        {
            if(t2 > TOP_N)  ///只有左孩子
                cmp = t1;
            else
                cmp = top[t1].val > top[t2].val ? t2 : t1;

            if(top[pos].val > top[cmp].val) ///pos保存在子孩子中，数值较小者的位置
            {
                ip = top[pos].ip; top[pos].ip = top[cmp].ip;
                val = top[pos].val; top[pos].val = top[cmp].val;
                top[cmp].ip = ip; top[cmp].val = val;
                pos = cmp;
            }
            else
                break;
        }
    }
}

#if 1
static void count_thread(void *arg)
{
    int i;
    ip_count_t *ict = (ip_count_t *)arg;
    ip_info *use = NULL, *head = NULL, *tail = NULL;
    unsigned long recv, send, inflow, outflow, session_total, http_session, tcp_flow, udp_flow, icmp_flow, http_flow;
    normal_value *normal;
    int attack_type;

    while(ict->stats)
    {
        lock(&(ict->add_lock));
        head = ict->use_head;
        tail = ict->use_tail;
        unlock(&(ict->add_lock));
        use = head;
        ict->time = base_time;
        //lock(&(ict->del_lock));
        lock(&(ict->top_lock));
        memset(ict->top, 0, sizeof(ict->top));
        while(use)
        {
            recv = use->recv;
            send = use->send;
            inflow = use->inflow;
            outflow = use->outflow;
            session_total = use->session_total;
            http_session = use->http_session;
            tcp_flow = use->tcp_flow;
            udp_flow = use->udp_flow;
            icmp_flow = use->icmp_flow;
            http_flow = use->http_flow;
            if(ict->time - use->last_time > 1000000)
            {
                unsigned long seconds = 0;
                if(!use->last_time || (ict->time - use->last_time < 2000000))    //take attention!     take what attention?
                {
                    seconds = 1;
                    use->pps_in = recv - use->last_recv;
                    use->pps_out = send - use->last_send;
                    use->bps_in = inflow - use->last_inflow;
                    use->bps_out = outflow - use->last_outflow;
                    use->new_session = session_total - use->last_session_total;
                    use->new_http_session = http_session - use->last_http_session;
                    use->tcp_bps = tcp_flow - use->last_tcp_flow;
                    use->udp_bps = udp_flow - use->last_udp_flow;
                    use->icmp_bps = icmp_flow - use->last_icmp_flow;
                    use->http_bps = http_flow - use->last_http_flow;
                }
                else
                {
                    seconds = (ict->time - use->last_time) / 1000000;
                    use->pps_in = (recv - use->last_recv) / seconds;
                    use->pps_out = (send - use->last_send) / seconds;
                    use->bps_in = (inflow - use->last_inflow) / seconds;
                    use->bps_out = (outflow - use->last_outflow) / seconds;
                    use->new_session = (session_total - use->last_session_total) / seconds;
                    use->new_http_session = (http_session - use->last_http_session) / seconds;
                    use->tcp_flow = (tcp_flow - use->last_tcp_flow) / seconds;
                    use->udp_flow = (udp_flow - use->last_udp_flow) / seconds;
                    use->icmp_bps = (icmp_flow - use->last_icmp_flow) / seconds;
                    use->http_bps = (http_flow - use->last_http_flow) / seconds;
                }
                use->last_recv = recv;
                use->last_send = send;
                use->last_inflow = inflow;
                use->last_outflow = outflow;
                use->last_session_total = session_total;
                use->last_http_session = http_session;
                use->last_tcp_flow = tcp_flow;
                use->last_udp_flow = udp_flow;
                use->last_icmp_flow = icmp_flow;
                use->last_http_flow = http_flow;
                use->last_time = ict->time;

                normal = &use->normal[NORMAL_VALUE_FLOW];
                if((!normal->value) || (use->bps_in < 20000000) || ((normal->value << 1) > use->bps_in) || (normal->time < 600))
                {
                    normal->ab_time = 0;
                    normal->total += use->bps_in;
                    normal->time += seconds;
                    normal->value = normal->total / normal->time;
                    attack_type = (use->attack & IPCOUNT_ATTACK_TCP_FLOOD) | (use->attack & IPCOUNT_ATTACK_UDP_FLOOD) | (use->attack & IPCOUNT_ATTACK_ICMP_FLOOD);
                    if(attack_type)
                    {
                        use->attack &= ~(attack_type);
                        if(ict->attack_cbk)
                        {
                            ipcount_attack_cbk callback = (ipcount_attack_cbk)ict->attack_cbk;
                            callback(ict, use->ip, attack_type, 0, use->attack_max_pps, use->attack_max_bps);
                        }
                    }
                }
                else
                {
                    normal->ab_time++;
                    if(normal->ab_time > 10)
                    {
                        if(use->tcp_bps > use->udp_bps)
                        {
                            if(use->tcp_bps > use->icmp_bps)
                                attack_type = IPCOUNT_ATTACK_TCP_FLOOD;
                            else
                                attack_type = IPCOUNT_ATTACK_ICMP_FLOOD;
                        }
                        else
                        {
                            if(use->udp_bps > use->icmp_bps)
                                attack_type = IPCOUNT_ATTACK_UDP_FLOOD;
                            else
                                attack_type = IPCOUNT_ATTACK_ICMP_FLOOD;
                        }
                        if(!(use->attack & attack_type))
                        {
                            use->attack |= attack_type;
                            if(ict->attack_cbk)
                            {
                                ipcount_attack_cbk callback = (ipcount_attack_cbk)ict->attack_cbk;
                                callback(ict, use->ip, attack_type, 1, use->attack_max_pps, use->attack_max_bps);
                            }
                        }
                    }
                }

                normal = &use->normal[NORMAL_VALUE_SESSION];
                if((!normal->value) || (use->new_session < 2500) || ((normal->value << 1) > use->new_session) || (normal->time < 600))
                {
                    normal->ab_time = 0;
                    normal->total += use->new_session;
                    normal->time += seconds;
                    normal->value = normal->total / normal->time;
                    if(use->attack & IPCOUNT_ATTACK_SYN_FLOOD)
                    {
                        use->attack &= ~(IPCOUNT_ATTACK_SYN_FLOOD);
                        if(ict->attack_cbk)
                        {
                            ipcount_attack_cbk callback = (ipcount_attack_cbk)ict->attack_cbk;
                            callback(ict, use->ip, IPCOUNT_ATTACK_SYN_FLOOD, 0, use->attack_max_pps, use->attack_max_bps);
                        }
                    }
                }
                else
                {
                    normal->ab_time++;
                    if(normal->ab_time > 10)
                    {
                        if(!(use->attack & IPCOUNT_ATTACK_SYN_FLOOD))
                        {
                            use->attack |= IPCOUNT_ATTACK_SYN_FLOOD;
                            if(ict->attack_cbk)
                            {
                                ipcount_attack_cbk callback = (ipcount_attack_cbk)ict->attack_cbk;
                                callback(ict, use->ip, IPCOUNT_ATTACK_SYN_FLOOD, 1, use->attack_max_pps, use->attack_max_bps);
                            }
                        }
                    }
                }
                if(!use->attack)
                    use->attack_max_pps = use->attack_max_bps = 0;
                else
                {
                    if(use->pps_in > use->attack_max_pps)
                        use->attack_max_pps = use->pps_in;
                    if(use->bps_in > use->attack_max_bps)
                        use->attack_max_bps = use->bps_in;
                }
            }
            if(1)
            {
                if(use->pps_in > ict->top[TOP_PPS_IN][1].val)
                {
                    ict->top[TOP_PPS_IN][1].ip = use->ip;
                    ict->top[TOP_PPS_IN][1].val = use->pps_in;
                    top_repeat(ict->top[TOP_PPS_IN]);
                }
                if(use->pps_out > ict->top[TOP_PPS_OUT][1].val)
                {
                    ict->top[TOP_PPS_OUT][1].ip = use->ip;
                    ict->top[TOP_PPS_OUT][1].val = use->pps_out;
                    top_repeat(ict->top[TOP_PPS_OUT]);
                }
                if(use->bps_in > ict->top[TOP_BPS_IN][1].val)
                {
                    ict->top[TOP_BPS_IN][1].ip = use->ip;
                    ict->top[TOP_BPS_IN][1].val = use->bps_in;
                    top_repeat(ict->top[TOP_BPS_IN]);
                }
                if(use->bps_out > ict->top[TOP_BPS_OUT][1].val)
                {
                    ict->top[TOP_BPS_OUT][1].ip = use->ip;
                    ict->top[TOP_BPS_OUT][1].val = use->bps_out;
                    top_repeat(ict->top[TOP_BPS_OUT]);
                }
                if(use->new_session > ict->top[TOP_NEW_SESSION][1].val)
                {
                    ict->top[TOP_NEW_SESSION][1].ip = use->ip;
                    ict->top[TOP_NEW_SESSION][1].val = use->new_session;
                    top_repeat(ict->top[TOP_NEW_SESSION]);
                }
                if(use->new_http_session > ict->top[TOP_NEW_HTTP][1].val)
                {
                    ict->top[TOP_NEW_HTTP][1].ip = use->ip;
                    ict->top[TOP_NEW_HTTP][1].val = use->new_http_session;
                    top_repeat(ict->top[TOP_NEW_HTTP]);
                }
                if(use->icmp_bps > ict->top[TOP_ICMP_BPS][1].val)
                {
                    ict->top[TOP_ICMP_BPS][1].ip = use->ip;
                    ict->top[TOP_ICMP_BPS][1].val = use->icmp_bps;
                    top_repeat(ict->top[TOP_ICMP_BPS]);
                }
                if(use->http_bps > ict->top[TOP_HTTP_BPS][1].val)
                {
                    ict->top[TOP_HTTP_BPS][1].ip = use->ip;
                    ict->top[TOP_HTTP_BPS][1].val = use->http_bps;
                    top_repeat(ict->top[TOP_HTTP_BPS]);
                }
            }
            if(use == tail)
                break;
            else
                use = use->next_use;
        }
        unlock(&(ict->top_lock));
        //unlock(&(ict->del_lock));
    }
}
#endif

static int ip_count_expand(ip_count_t *ict)
{
    ip_info *buf = NULL;
    if(ict->buf_total >= MAX_COUNT_BUF)
        return 0;
    buf = (ip_info *)malloc(IP_EACH_BUF_SIZE * sizeof(ip_info));
    if(buf)
    {
        int i;
        memset(buf, 0, IP_EACH_BUF_SIZE * sizeof(ip_info));
        for(i = 0; i < IP_EACH_BUF_SIZE; i++)
        {
            if(ict->alive)
                ict->alive->next_alive = &buf[i];
            ict->alive = &buf[i];
        }
        if(!ict->cur)
            ict->cur = buf;
        ict->buf[ict->buf_total++] = buf;
        return 1;
    }
    return 0;
}

ip_count_t *ipcount_init()
{
    ip_count_t *ict = NULL;
    ict = (ip_count_t *)malloc(sizeof(ip_count_t));
    if(ict)
    {
        memset(ict, 0, sizeof(ip_count_t));
        if(ip_count_expand(ict))
        {
            ict->stats = 1;
            ict->table = (ip_info **)malloc(IP_HASH_SIZE * sizeof(ip_info *));
            ict->table_lock = (unsigned char *)malloc(IP_HASH_SIZE * sizeof(char));
            memset(ict->table, 0, IP_HASH_SIZE * sizeof(ip_info *));
            memset(ict->table_lock, 0, IP_HASH_SIZE * sizeof(char));
            //pthread_create(&(ict->timer), NULL, count_timer, (void *)ict);
            pthread_create(&(ict->counter), NULL, count_thread, (void *)ict);
        }
        else
        {
            free(ict);
            ict = NULL;
        }
    }
    return ict;
}

int ipcount_tini(ip_count_t *ict)
{
    if(ict)
    {
        int i;
        ict->stats = 0;
        //pthread_join(ict->timer, NULL);
        pthread_join(ict->counter, NULL);
        for(i = 0; i < ict->buf_total; i++)
            free(ict->buf[i]);
        if(ict->table)
            free(ict->table);
        if(ict->table_lock)
            free(ict->table_lock);
        free(ict);
        return 1;
    }
    return 0;
}

int ipcount_set_attack_cbk(ip_count_t *ict, void *cbk)
{
    if(ict && ict->stats)
    {
        ict->attack_cbk = cbk;
        return 1;
    }
    return 0;
}

int ipcount_add_ip(ip_count_t *ict, unsigned int ip)
{
    int ret = 0;
    if(ict && ict->stats && ip)
    {
        //unsigned int key = ip % IP_HASH_SIZE;
        unsigned int key = ((ip & 0x0fffffff) >> 4) % IP_HASH_SIZE;
        ip_info *find = NULL, *prev = NULL;

        lock(&(ict->table_lock[key]));
        find = ict->table[key];
        while(find)
        {
            if(find->ip == ip)
                break;
            prev = find;
            find = find->next;
        }
        if(!find)
        {
            lock(&(ict->add_lock));
            if(!ict->cur)
                ip_count_expand(ict);
            if(ict->cur)
            {
                find = ict->cur;
                ict->cur = ict->cur->next_alive;
                memset(find, 0, sizeof(ip_info));
                find->ip = ip;
                if(prev)
                {
                    prev->next = find;
                    find->prev = prev;
                }
                else
                    ict->table[key] = find;
                if(ict->use_tail)
                {
                    ict->use_tail->next_use = find;
                    find->prev_use = ict->use_tail;
                }
                ict->use_tail = find;
                if(!ict->use_head)
                    ict->use_head = find;
                ict->use_total++;
                ret = 1;
            }
            unlock(&(ict->add_lock));
        }
        unlock(&(ict->table_lock[key]));
    }
    return ret;
}

int ipcount_del_ip(ip_count_t *ict, unsigned int ip)      // think about the data link!!
{
    int ret = 0;
    if(0)//ict && ict->stats && ip)
    {
        //unsigned int key = ip % IP_HASH_SIZE;
        unsigned int key = ((ip & 0x0fffffff) >> 4) % IP_HASH_SIZE;
        ip_info *find = NULL;

        lock(&(ict->table_lock[key]));
        find = ict->table[key];
        while(find)
        {
            if(find->ip == ip)
                break;
            find = find->next;
        }
        if(find)
        {
            lock(&(ict->del_lock));
            if(ict->use_head == find)
            {
                ict->use_head = find->next_use;
                if(ict->use_head)
                    ict->use_head->prev_use = NULL;
            }
            if(ict->use_tail == find)
            {
                ict->use_tail = find->prev_use;
                if(ict->use_tail)
                    ict->use_tail->next_use = NULL;
            }
            if(find->prev_use)
            {
                find->prev_use->next_use = find->next_use;
                if(find->next_use)
                    find->next_use->prev_use = find->prev_use;
            }
            if(find->prev)
            {
                find->prev->next = find->next;
                if(find->next)
                    find->next->prev = find->prev;
            }
            else
            {
                ict->table[key] = find->next;
                if(find->next)
                    find->next->prev = NULL;
            }
            if(ict->alive)
                ict->alive->next_alive = find;
            ict->alive = find;
            if(!ict->cur)
                ict->cur = find;
            ict->use_total--;
            ret = 1;
            unlock(&(ict->del_lock));
        }
        unlock(&(ict->table_lock[key]));
    }
    return ret;
}

int ipcount_add_pkg(ip_count_t *ict, void *pkg, unsigned int len, unsigned char add_ip_flag, unsigned int session_type)
{
    int find_level = 0;
    if(ict && ict->stats && pkg && IF_IP(pkg) && len)
    {
        unsigned int skey, dkey;
        unsigned int sip = GET_IP_SIP(pkg);
        unsigned int dip = GET_IP_DIP(pkg);
        ip_info *sfind = NULL, *dfind = NULL, *find = NULL;
        unsigned long time = base_time;

    finding:
        //skey = sip % IP_HASH_SIZE;
        //dkey = dip % IP_HASH_SIZE;
        skey = ((sip & 0x0fffffff) >> 4) % IP_HASH_SIZE;
        dkey = ((dip & 0x0fffffff) >> 4) % IP_HASH_SIZE;
        sfind = ict->table[skey];
        dfind = ict->table[dkey];
        if(!add_ip_flag || (add_ip_flag & IPCOUNT_ADD_FLAG_SIP))
        {
            lock(&(ict->table_lock[skey]));
            while(!find && sfind)
            {
                if(sfind->ip == sip)
                {
                    find = sfind;
                    break;
                }
                sfind = sfind->next;
                find_level++;
            }
            unlock(&(ict->table_lock[skey]));
        }
        if(find_level > ict->max_level)
            ict->max_level = find_level;
        if(!add_ip_flag || (add_ip_flag & IPCOUNT_ADD_FLAG_DIP))
        {
            lock(&(ict->table_lock[dkey]));
            while(!find && dfind)
            {
                if(dfind->ip == dip)
                {
                    find = dfind;
                    break;
                }
                dfind = dfind->next;
                find_level++;
            }
            unlock(&(ict->table_lock[dkey]));
        }
        if(find_level > ict->max_level)
            ict->max_level = find_level;
        if(find)
        {
            if(find == sfind)
            {
                find->send++;
                find->outflow += len;
            }
            else
            {
                find->recv++;
                find->inflow += len;
            }
            if(IF_TCP(pkg))
                find->tcp_flow += len;
            else if(IF_UDP(pkg))
                find->udp_flow += len;
            else if(IF_ICMP(pkg))
                find->icmp_flow += len;
            if(session_type == IPCOUNT_SESSION_TYPE_HTTP)
                find->http_flow += len;
            #if 0
            if(time - find->last_time >= 1000000)
            {
                unsigned long seconds = (time - find->last_time) / 1000000;
                find->pps_in = (find->recv - find->last_recv) / seconds;
                find->pps_out = (find->send - find->last_send) / seconds;
                find->bps_in = (find->inflow - find->last_inflow) / seconds;
                find->bps_out = (find->outflow - find->last_outflow) / seconds;
                find->last_recv = find->recv;
                find->last_send = find->send;
                find->last_inflow = find->inflow;
                find->last_outflow = find->outflow;
                find->last_time = time;
            }
            #endif
            return 1;
        }
        else if(add_ip_flag)
        {
            if(add_ip_flag & IPCOUNT_ADD_FLAG_SIP)
                ipcount_add_ip(ict, sip);
            if(add_ip_flag & IPCOUNT_ADD_FLAG_DIP)
                ipcount_add_ip(ict, dip);
        }
    }
    return 0;
}

int ipcount_add_session(ip_count_t *ict, unsigned int sip, unsigned int dip, unsigned int session_type)
{
    if(ict && ict->stats && sip && dip)
    {
        unsigned int skey, dkey;
        ip_info *sfind = NULL, *dfind = NULL, *find = NULL;
    finding:
        skey = ((sip & 0x0fffffff) >> 4) % IP_HASH_SIZE;
        dkey = ((dip & 0x0fffffff) >> 4) % IP_HASH_SIZE;
        sfind = ict->table[skey];
        dfind = ict->table[dkey];
        lock(&(ict->table_lock[skey]));
        while(!find && sfind)
        {
            if(sfind->ip == sip)
            {
                find = sfind;
                break;
            }
            sfind = sfind->next;
        }
        unlock(&(ict->table_lock[skey]));
        lock(&(ict->table_lock[dkey]));
        while(!find && dfind)
        {
            if(dfind->ip == dip)
            {
                find = dfind;
                break;
            }
            dfind = dfind->next;
        }
        unlock(&(ict->table_lock[dkey]));
        if(find)
        {
            switch(session_type)
            {
                case IPCOUNT_SESSION_TYPE_NEW:
                    find->session_total++;
                    break;
                case IPCOUNT_SESSION_TYPE_CLOSE:
                    find->session_close++;
                    break;
                case IPCOUNT_SESSION_TYPE_TIMEOUT:
                    find->session_timeout++;
                    break;
                case IPCOUNT_SESSION_TYPE_HTTP:
                    find->http_session++;
                    break;
                case IPCOUNT_SESSION_TYPE_UNKNOW:
                    break;
                default:;
            }
            return 1;
        }
    }
    return 0;
}

int ipcount_get_ip(ip_count_t *ict, ip_data *id)
{
    int ret = 0;
    if(ict && ict->stats && id && id->ip)
    {
        unsigned int key = ((id->ip & 0x0fffffff) >> 4) % IP_HASH_SIZE;
        ip_info *find = NULL;
        lock(&(ict->table_lock[key]));
        find = ict->table[key];
        while(find)
        {
            if(find->ip == id->ip)
                break;
            find = find->next;
        }
        unlock(&(ict->table_lock[key]));
        if(find)
        {
            id->attack_max_pps = find->attack_max_pps;
            id->attack_max_bps = find->attack_max_bps;
            id->recv = find->recv;
            id->send = find->send;
            id->inflow = find->inflow;
            id->outflow = find->outflow;
            id->tcp_flow = find->tcp_flow;
            id->udp_flow = find->udp_flow;
            id->icmp_flow = find->icmp_flow;
            id->http_flow = find->http_flow;
            id->session_total = find->session_total;
            id->session_close = find->session_close;
            id->session_timeout = find->session_timeout;
            id->http_session = find->http_session;
            ret = 1;
        }
    }
    return ret;
}

int ipcount_get_ip_total(ip_count_t *ict)
{
    if(ict && ict->stats)
        return ict->use_total;
    return 0;
}

int ipcount_get_top_ip(ip_count_t *ict, int top_flag, top_data *td, unsigned int total)
{
    unsigned int ret = total;
    int i;
    top_info *top = NULL;
    switch(top_flag)
    {
        case IPCOUNT_TOP_PPS_IN:
            top = ict->top[TOP_PPS_IN];
            break;
        case IPCOUNT_TOP_PPS_OUT:
            top = ict->top[TOP_PPS_OUT];
            break;
        case IPCOUNT_TOP_BPS_IN:
            top = ict->top[TOP_BPS_IN];
            break;
        case IPCOUNT_TOP_BPS_OUT:
            top = ict->top[TOP_BPS_OUT];
            break;
        case IPCOUNT_TOP_NEW_SESSION:
            top = ict->top[TOP_NEW_SESSION];
            break;
        case IPCOUNT_TOP_NEW_HTTP:
            top = ict->top[TOP_NEW_HTTP];
            break;
        case IPCOUNT_TOP_ICMP_BPS:
            top = ict->top[TOP_ICMP_BPS];
            break;
        case IPCOUNT_TOP_HTTP_BPS:
            top = ict->top[TOP_HTTP_BPS];
            break;
        default:;
    }
    if(ict && ict->stats && td && total && top)
    {
        lock(&(ict->top_lock));
        for(i = 1; i <= TOP_N; i++)
        {
            if(top[i].ip)
            {
                td->ip = top[i].ip;
                td->val = top[i].val;
                td++;
                total--;
            }
        }
        unlock(&(ict->top_lock));
    }
    return ret - total;
}

int ipcount_get_all_ip(ip_count_t *ict, ip_data *id, unsigned int total)
{
    unsigned int ret = total;
    if(ict && ict->stats && id && total)
    {
        ip_info *use = NULL, *head = NULL, *tail = NULL;
        lock(&(ict->add_lock));
        head = ict->use_head;
        tail = ict->use_tail;
        unlock(&(ict->add_lock));
        use = head;
        //lock(&(ict->del_lock));
        while(use && total)
        {
            id->ip = use->ip;
            id->recv = use->recv;
            id->send = use->send;
            id->inflow = use->inflow;
            id->outflow = use->outflow;
            id->pps_in = use->pps_in;
            id->pps_out = use->pps_out;
            id->bps_in = use->bps_in;
            id->bps_out = use->bps_out;
            id->tcp_flow = use->tcp_flow;
            id->udp_flow = use->udp_flow;
            id->icmp_flow = use->icmp_flow;
            id->http_flow = use->http_flow;
            id->session_total = use->session_total;
            id->session_close = use->session_close;
            id->session_timeout = use->session_timeout;
            id->http_session = use->http_session;
            id++;
            total--;
            if(use == tail)
                break;
            else
                use = use->next_use;
        }
        //unlock(&(ict->del_lock));
    }
done:
    return ret - total;
}
