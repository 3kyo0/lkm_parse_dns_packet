// ©.
// https://github.com/sizet/lkm_parse_dns_packet

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/ip.h>
#include <linux/udp.h>




#define FILE_NAME (strrchr(__FILE__, '/') ? strrchr(__FILE__, '/') + 1 : __FILE__)
#define DMSG(msg_fmt, msg_args...) \
    printk(KERN_INFO "%s(%04u): " msg_fmt "\n", FILE_NAME, __LINE__, ##msg_args)




// class 定義.
// https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-2
enum CLASS_LIST
{
    CLASS_IN = 1,
    CLASS_RESERVED = 65535
};

// RR-type 定義.
// https://www.iana.org/assignments/dns-parameters/dns-parameters.xhtml#dns-parameters-4
enum RR_TYPE_LIST
{
    RR_TYPE_A = 1,
    RR_TYPE_CNAME = 5,
    RR_TYPE_AAAA = 28,
    RR_TYPE_RESERVED = 65535
};




// DNS 封包欄位.
struct dnshdr
{
    __be16 id;
#if defined (__LITTLE_ENDIAN_BITFIELD)
    __u8 rd:1,
         tc:1,
         aa:1,
         opcode:4,
         qr:1;
    __u8 rcode:4,
         z:3,
         ra:1;
#elif defined (__BIG_ENDIAN_BITFIELD)
    __u8 qr:1,
         opcode:4,
         aa:1,
         tc:1,
         rd:1;
    __u8 ra:1,
         z:3,
         rcode:4;
#else
#error "unknown endian type"
#endif
    __be16 qdcount;
    __be16 ancount;
    __be16 nscount;
    __be16 arcount;
} __attribute__((packed));

// DNS 封包的 question section 欄位.
struct dns_question_section
{
    char *qname;
    __u16 qtype;
    __u16 qclass;
};

// DNS 封包的 answer section 欄位.
struct dns_answer_section
{
    char *name;
    __u16 type;
    __u16 class;
    __u32 ttl;
    __u16 rdlength;
    void *rdata;
};




struct nf_hook_ops nf_hook_inet_local_in, nf_hook_inet_local_out;




// 分析 FQDN.
static size_t parse_name(
    struct dnshdr *dns_hdr,
    char *name_field,
    char *name_buf,
    size_t buf_size,
    size_t *name_len_buf,
    unsigned int in_recursive)
{
    /*
    DNS name 格式 :

    無壓縮, 範例 : tw.yahoo.com
    | 2 | t | w | 5 | y | a | h | o | o | 3 | c | o | m | \0 |

    使用壓縮時, 使用指針指向之前出現的域名的位置.
    指針的大小為 2byte, 最高 2bit 為 11 (11xxxxxxxxxxxxxx),
    剩餘的 10bit 紀錄以 DNS 封包開頭為基底往後偏移,

    解壓縮時, 將 2byte 資料和 0x3FFF 做 AND 運算, 得到的值再以 DNS 封包開頭為基底往後偏移,
    就是之前出現的域名的位置.

    範例-01 (全部壓縮) :
    第一個域名為 tw.yahoo.com, 則域名欄位為 :
    | 2 | t | w | 5 | y | a | h | o | o | 3 | c | o | m | \0 |
    假設 | 2 | 的偏移植是 0x0E
    第二個域名和第一個域名相同, 則域名欄位為 :
    | 0xC00E |
    (0xC000 | 0x000E = 0xC00E)

    範例-02 (部分壓縮) :
    第一個域名為 tw.yahoo.com, 則域名欄位為 :
    | 2 | t | w | 5 | y | a | h | o | o | 3 | c | o | m | \0 |
    假設 | 5 | 的偏移植是 0x0F
    第二個域名為 www.yahoo.com, 壓縮 yahoo.com, 則域名欄位為 :
    | 3 | w | w | w | 0xC00F |

    範例-03 (多重壓縮) :
    第一個域名為 tw.yahoo.com, 則域名欄位為 :
    | 2 | t | w | 5 | y | a | h | o | o | 3 | c | o | m | \0 |
    假設 | 5 | 的偏移植是 0x000F
    第二個域名為 www.yahoo.com, 壓縮 yahoo.com, 則域名欄位為 :
    | 3 | w | w | w | 0xC00F |
    假設 | 3 | 的偏移植是 0x001D
    第三個域名和第二個相同, 則域名欄位為 :
    | 0xC01D |

    注意事項 :
    一個域名欄位只能有一個指針, 指針後面不可有域名字串, 指針後面不需要加上終止字元 '\0'.
    範例 :
    | 0xC01D | = 正確.
    | 3 | w | w | w | 0xC00F | = 正確.
    | 0xC00E | 4 | r | o | o | t | \0 | = 錯誤 (指針後面不可有域名字串).
    */

    __u8 *link_name;
    size_t name_offset;
    // 紀錄域名欄位的長度.
    size_t flen = 0;
    // 紀錄域名的長度.
    size_t nlen = 0, plen = 0;
    // 紀錄標籤開頭的長度資料.
    size_t llen = 0;


    // 扣掉終止字元 '\0'
    if(in_recursive == 0)
        buf_size--;

    for(; *name_field != '\0'; name_field++)
    {
        // 檢查是否使用壓縮.
        if((*name_field & 0xC0) == 0xC0)
        {
            // 取得偏移植.
            name_offset = ntohs(*((__be16 *) name_field)) & 0x3FFF;
            // 找到目標域名欄位.
            link_name = ((__u8 *) dns_hdr) + name_offset;

            // 取出參考的域名.
            parse_name(dns_hdr, link_name, name_buf + nlen, buf_size - nlen, &plen, 1);
            nlen += plen;

            // 域名欄位長度加上指針大小.
            flen += 2;
            // 指針後面不可有域名字串, 離開處理.
            break;
        }

        // 紀錄域名資料.
        // flen > 0 表示跳過開頭紀錄標籤長度的位元.
        // 例如 :
        // | 2 | t | w | 5 | y | a | h | o | o | 3 | c | o | m | \0 |
        // 跳過 | 2 |.
        if(flen > 0)
            if(nlen < buf_size)
            {
                // llen == 0 表示遇到紀錄標籤長度的位元, 改用 '.' 替代.
                // 例如 :
                // | 2 | t | w | 5 | y | a | h | o | o | 3 | c | o | m | \0 |
                // 遇到 | 2 | | 5 | | 3 |.
                name_buf[nlen] = llen == 0 ? '.' : *name_field;
                nlen++;
            }

        // llen == 0 表示遇到紀錄標籤長度的位元, 紀錄之後的標籤的長度,
        // llen != 0 表示遇到標籤字串, 標籤長度減 1.
        llen = llen == 0 ? *name_field : llen - 1;

        flen++;
    }

    name_buf[nlen] = '\0';

    *name_len_buf = nlen;

    return flen;
}

// 分析 question section.
static size_t parse_question_section(
    struct dnshdr *dns_hdr,
    __u8 *section_loc)
{
    size_t slen = 0;
    void *data_offset;
    char name_buf[256];
    size_t name_len;
    struct dns_question_section dns_qd;


    name_buf[0] = '\0';

    memset(&dns_qd, 0, sizeof(dns_qd));

    data_offset = section_loc;
    slen += parse_name(dns_hdr, (char *) data_offset, name_buf, sizeof(name_buf), &name_len, 0);
    DMSG("qname = %s", name_buf);

    data_offset = section_loc + slen;
    dns_qd.qtype = ntohs(*((__be16 *) data_offset));
    DMSG("qtype = 0x%04X", dns_qd.qtype);
    slen += sizeof(dns_qd.qtype);

    data_offset = section_loc + slen;
    dns_qd.qclass = ntohs(*((__be16 *) data_offset));
    DMSG("qclass = 0x%04X", dns_qd.qclass);
    slen += sizeof(dns_qd.qclass);

    return slen;
}

// 分析 answer section.
static size_t parse_answer_section(
    struct dnshdr *dns_hdr,
    __u8 *section_loc)
{
    size_t slen = 0;
    void *data_offset;
    char name_buf[256];
    size_t name_len;
    struct dns_answer_section dns_an;


    name_buf[0] = '\0';

    memset(&dns_an, 0, sizeof(dns_an));

    data_offset = section_loc;
    slen += parse_name(dns_hdr, (char *) data_offset, name_buf, sizeof(name_buf), &name_len, 0);
    DMSG("name = %s", name_buf);

    data_offset = section_loc + slen;
    dns_an.type = ntohs(*((__be16 *) data_offset));
    DMSG("type = 0x%04X", dns_an.type);
    slen += sizeof(dns_an.type);

    data_offset = section_loc + slen;
    dns_an.class = ntohs(*((__be16 *) data_offset));
    DMSG("class = 0x%04X", dns_an.class);
    slen += sizeof(dns_an.class);

    data_offset = section_loc + slen;
    dns_an.ttl = ntohl(*((__be32 *) data_offset));
    DMSG("ttl = %u", dns_an.ttl);
    slen += sizeof(dns_an.ttl);

    data_offset = section_loc + slen;
    dns_an.rdlength = ntohs(*((__be16 *) data_offset));
    DMSG("rdlength = %u", dns_an.rdlength);
    slen += sizeof(dns_an.rdlength);

    dns_an.rdata = section_loc + slen;
    slen += dns_an.rdlength;

    // 回應的資料.
    if(dns_an.class == CLASS_IN)
    {
        if(dns_an.type == RR_TYPE_A)
        {
            DMSG("rdata (IPv4) = %pi4", dns_an.rdata);
        }
        else
        if(dns_an.type == RR_TYPE_AAAA)
        {
            DMSG("rdata (IPv6) = %pi6", dns_an.rdata);
        }
        else
        if(dns_an.type == RR_TYPE_CNAME)
        {
            parse_name(dns_hdr, (char *) dns_an.rdata, name_buf, sizeof(name_buf), &name_len, 0);
            DMSG("rdata (CNAME) = %s", name_buf);
        }
    }

    return slen;
}

// 分析 DNS 封包.
static void parse_dns(
    struct dnshdr *dns_hdr)
{
    size_t qdcnt, ancnt, sidx, scnt = 0;
    __u8 *section_loc;


    // 找出 question counter 和 answer counter.
    qdcnt = ntohs(dns_hdr->qdcount);
    ancnt = ntohs(dns_hdr->ancount);

    DMSG("");
    DMSG("========================");
    DMSG("");
    DMSG("DNS %s", dns_hdr->qr == 0 ? "query" : "response");

    DMSG("question count = %zd", qdcnt);
    DMSG("answer count = %zd", ancnt);

    // 移動到 section 部分.
    section_loc = ((__u8 *) dns_hdr) + sizeof(struct dnshdr);

    // 分析 question section.
    for(sidx = 0; sidx < qdcnt; sidx++)
    {
        DMSG("");
        DMSG("question section %zd", sidx + 1);
        scnt = parse_question_section(dns_hdr, section_loc);
        section_loc += scnt;
    }

    // 分析 answer section.
    for(sidx = 0; sidx < ancnt; sidx++)
    {
        DMSG("");
        DMSG("answer section %zd", sidx + 1);
        scnt = parse_answer_section(dns_hdr, section_loc);
        section_loc += scnt;
    }

    return;
}

// 檢查哪些 DNS 封包要分析.
static void check_dns(
    struct sk_buff *skb,
    unsigned int packet_direct)
{
    struct iphdr *ip4_hdr;
    struct udphdr *udp_hdr;
    struct dnshdr *dns_hdr;
    __be16 tmp_port;


    ip4_hdr = ip_hdr(skb);

    // 只分析 UDP 類型的 DNS 封包.
    if(ip4_hdr->protocol != IPPROTO_UDP)
        return;

    udp_hdr = (struct udphdr *) (((__u8 *) ip4_hdr) + (ip4_hdr->ihl * 4));

    dns_hdr = (struct dnshdr *) (((__u8 *) udp_hdr) + sizeof(struct udphdr));

    // 只分析埠號 53 的 DNS 封包.
    tmp_port = packet_direct == NF_INET_LOCAL_OUT ? udp_hdr->dest : udp_hdr->source;
    if(tmp_port != __constant_htons(53))
        return;

    // 只分析標準查詢.
    if(dns_hdr->opcode != 0)
        return;

    // 只分析沒有錯誤 (response code = 0) 的 DNS 封包.
    if(dns_hdr->rcode != 0)
        return;

    // 只分析有提出查詢 (question count > 0) 的 DNS 封包.
    if(dns_hdr->qdcount == 0)
        return;

    // 處理要分析的 DNS 封包.
    parse_dns(dns_hdr);

    return;
}

#if KERNEL_VERSION(4, 4, 0) <= LINUX_VERSION_CODE
// 4.4.0 <= kernel
static unsigned int handle_dns_hook(
    void *priv,
    struct sk_buff *skb,
    const struct nf_hook_state *state)
{
    check_dns(skb, state->hook);

    return NF_ACCEPT;
}
#elif (KERNEL_VERSION(4, 1, 0) <= LINUX_VERSION_CODE) && \
      (LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0))
// 4.1.0 <= kernel < 4.4.0
static unsigned int handle_dns_hook(
    const struct nf_hook_ops *ops,
    struct sk_buff *skb,
    const struct nf_hook_state *state)
{
    check_dns(skb, ops->hooknum);

    return NF_ACCEPT;
}
#elif (KERNEL_VERSION(3, 13, 0) <= LINUX_VERSION_CODE) && \
      (LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 0))
// 3.13.0 <= kernel < 4.1.0
static unsigned int handle_dns_hook(
    const struct nf_hook_ops *ops,
    struct sk_buff *skb,
    const struct net_device *in,
    const struct net_device *out,
    int (*okfn) (struct sk_buff *))
{
    check_dns(skb, ops->hooknum);

    return NF_ACCEPT;
}
#else
// kernel < 3.13.0
static unsigned int handle_dns_hook(
    unsigned int hooknum,
    struct sk_buff *skb,
    const struct net_device *in,
    const struct net_device *out,
    int (*okfn) (struct sk_buff *))
{
    check_dns(skb, hooknum);

    return NF_ACCEPT;
}
#endif

static int __init main_init(
    void)
{
    // 註冊 IPv4 hook (NF_INET_LOCAL_OUT), 攔截 DNS 請求封包.
    memset(&nf_hook_inet_local_out, 0, sizeof(nf_hook_inet_local_out));
    nf_hook_inet_local_out.pf = PF_INET;
    nf_hook_inet_local_out.hooknum = NF_INET_LOCAL_OUT;
    nf_hook_inet_local_out.priority = NF_IP_PRI_FIRST;
    nf_hook_inet_local_out.hook = handle_dns_hook;
#if KERNEL_VERSION(4, 13, 0) <= LINUX_VERSION_CODE
    if(nf_register_net_hook(&init_net, &nf_hook_inet_local_out) < 0)
    {
        DMSG("call nf_register_net_hook(NF_INET_LOCAL_OUT) fail");
        goto FREE_02;
    }
#else
    if(nf_register_hook(&nf_hook_inet_local_out) < 0)
    {
        DMSG("call nf_register_hook(NF_INET_LOCAL_OUT) fail");
        goto FREE_02;
    }
#endif

    // 註冊 IPv4 hook (NF_INET_LOCAL_IN), 攔截 DNS 回應封包.
    memset(&nf_hook_inet_local_in, 0, sizeof(nf_hook_inet_local_in));
    nf_hook_inet_local_in.pf = PF_INET;
    nf_hook_inet_local_in.hooknum = NF_INET_LOCAL_IN;
    nf_hook_inet_local_in.priority = NF_IP_PRI_FIRST;
    nf_hook_inet_local_in.hook = handle_dns_hook;
#if KERNEL_VERSION(4, 13, 0) <= LINUX_VERSION_CODE
    if(nf_register_net_hook(&init_net, &nf_hook_inet_local_in) < 0)
    {
        DMSG("call nf_register_net_hook(NF_INET_LOCAL_IN) fail");
        goto FREE_01;
    }
#else
    if(nf_register_hook(&nf_hook_inet_local_in) < 0)
    {
        DMSG("call nf_register_hook(NF_INET_LOCAL_IN) fail");
        goto FREE_01;
    }
#endif

    return 0;
FREE_02:
#if KERNEL_VERSION(4, 13, 0) <= LINUX_VERSION_CODE
    nf_unregister_net_hook(&init_net, &nf_hook_inet_local_in);
#else
    nf_unregister_hook(&nf_hook_inet_local_in);
#endif
FREE_01:
    return 0;
}

static void __exit main_exit(
    void)
{
#if KERNEL_VERSION(4, 13, 0) <= LINUX_VERSION_CODE
    nf_unregister_net_hook(&init_net, &nf_hook_inet_local_in);
    nf_unregister_net_hook(&init_net, &nf_hook_inet_local_out);
#else
    nf_unregister_hook(&nf_hook_inet_local_in);
    nf_unregister_hook(&nf_hook_inet_local_out);
#endif

    return;
}

module_init(main_init);
module_exit(main_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Che-Wei Hsu");
MODULE_DESCRIPTION("Parse DNS packet");
