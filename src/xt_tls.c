#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <linux/string.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/inet.h>
#include <asm/errno.h>

#include "compat.h"
#include "hashmap.c"
#include "xt_tls.h"

HASHMAP_FUNCS_CREATE(flow, __u32, flow_data)

struct hashmap flow_map;

/*
 * Parse the client hello looking for
 * the SNI extension.
 */
static Result parse_chlo(flow_data *flow, char **dest)
{
	u_int offset, base_offset = 43, extension_offset = 2;
	u_int16_t session_id_len, cipher_len, compression_len, extensions_len;
	__u16 tls_header_len = (flow->data[3] << 8) + flow->data[4] + 5;

	if (base_offset + 2 > flow->data_len) {
#ifdef XT_TLS_DEBUG
		printk("[xt_tls] Data length is too small (%d)\n", (int)data_len);
#endif
		return NOT_ENOUGH_DATA;
	}

	// Get the length of the session ID
	session_id_len = flow->data[base_offset];

#ifdef XT_TLS_DEBUG
	printk("[xt_tls] Session ID length: %d\n", session_id_len);
#endif
	if ((session_id_len + base_offset + 2) > tls_header_len) {
#ifdef XT_TLS_DEBUG
		printk("[xt_tls] TLS header length is smaller than session_id_len + base_offset +2 (%d > %d)\n", (session_id_len + base_offset + 2), tls_header_len);
#endif
		return PROTOCOL_ERROR;
	}

	// Get the length of the ciphers
	memcpy(&cipher_len, &flow->data[base_offset + session_id_len + 1], 2);
	cipher_len = ntohs(cipher_len);
	offset = base_offset + session_id_len + cipher_len + 2;
#ifdef XT_TLS_DEBUG
	printk("[xt_tls] Cipher len: %d\n", cipher_len);
	printk("[xt_tls] Offset (1): %d\n", offset);
#endif
	if (offset > tls_header_len) {
#ifdef XT_TLS_DEBUG
		printk("[xt_tls] TLS header length is smaller than offset (%d > %d)\n", offset, tls_header_len);
#endif
		return PROTOCOL_ERROR;
	}

	// Get the length of the compression types
	compression_len = flow->data[offset + 1];
	offset += compression_len + 2;
#ifdef XT_TLS_DEBUG
	printk("[xt_tls] Compression length: %d\n", compression_len);
	printk("[xt_tls] Offset (2): %d\n", offset);
#endif
	if (offset > tls_header_len) {
#ifdef XT_TLS_DEBUG
		printk("[xt_tls] TLS header length is smaller than offset w/compression (%d > %d)\n", offset, tls_header_len);
#endif
		return PROTOCOL_ERROR;
	}

	// Get the length of all the extensions
	memcpy(&extensions_len, &flow->data[offset], 2);
	extensions_len = ntohs(extensions_len);
#ifdef XT_TLS_DEBUG
	printk("[xt_tls] Extensions length: %d\n", extensions_len);
#endif

	if ((extensions_len + offset) > tls_header_len) {
#ifdef XT_TLS_DEBUG
		printk("[xt_tls] TLS header length is smaller than offset w/extensions (%d > %d)\n", (extensions_len + offset), tls_header_len);
#endif
		return PROTOCOL_ERROR;
	}

	// Loop through all the extensions to find the SNI extension
	while (extension_offset < extensions_len)
	{
		u_int16_t extension_id, extension_len;

		memcpy(&extension_id, &flow->data[offset + extension_offset], 2);
		extension_offset += 2;

		memcpy(&extension_len, &flow->data[offset + extension_offset], 2);
		extension_offset += 2;

		extension_id = ntohs(extension_id), extension_len = ntohs(extension_len);

#ifdef XT_TLS_DEBUG
		printk("[xt_tls] Extension ID: %d\n", extension_id);
		printk("[xt_tls] Extension length: %d\n", extension_len);
#endif

		if (extension_id == 0) {
			u_int16_t name_length, name_type;

			// We don't need the server name list length, so skip that
			extension_offset += 2;
			// We don't really need name_type at the moment
			// as there's only one type in the RFC-spec.
			// However I'm leaving it in here for
			// debugging purposes.
			name_type = flow->data[offset + extension_offset];
			extension_offset += 1;

			memcpy(&name_length, &flow->data[offset + extension_offset], 2);
			name_length = ntohs(name_length);
			extension_offset += 2;

#ifdef XT_TLS_DEBUG
			printk("[xt_tls] Name type: %d\n", name_type);
			printk("[xt_tls] Name length: %d\n", name_length);
#endif
			// Allocate an extra byte for the null-terminator
			*dest = kmalloc(name_length + 1, GFP_KERNEL);
			strncpy(*dest, &flow->data[offset + extension_offset], name_length);
			// Make sure the string is always null-terminated.
			(*dest)[name_length] = 0;

			return NAME_FOUND;
		}

		extension_offset += extension_len;
	}

	return NAME_NOT_FOUND;
}

/*
 *
 */
static Result parse_server_cert(flow_data *data, char **dest)
{
	return NAME_NOT_FOUND;
}

/*
 * Searches through skb->data and looks for a
 * client or server handshake. A client
 * handshake is preferred as the SNI
 * field tells us what domain the client
 * wants to connect to.
 */
static int get_tls_hostname(const struct sk_buff *skb, char **dest)
{
	struct tcphdr *tcp_header = (struct tcphdr *)skb_transport_header(skb);
	__u8 handshake_protocol;
	__u16 header_offset = 0, tls_header_len;
	__u32 hash = skb_get_hash_raw(skb);
	flow_data *flow = flow_hashmap_get(&flow_map, &hash);

	/*
	 * If we have data for this flow, reallocate enough memory to combine the payloads,
	 * and copy the new payload to flow->data. This way we'll have all the data we need.
	 * If we don't have a flow already, allocate memory for one and put the data into it.
	 */
	if (flow != NULL)
	{
#ifdef XT_TLS_DEBUG
		printk("[xt_tls] flow found");
#endif
		__u8 *skb_payload = (__u8 *)tcp_header + (tcp_header->doff * 4);
		// Calculate packet payload length
		__u16 skb_payload_len = skb_tail_pointer(skb) - skb_payload;
		flow->data = krealloc(flow->data, skb_payload_len + flow->data_len, GFP_KERNEL);
		memcpy(flow->data + flow->data_len, skb_payload, skb_payload_len);
	} else {
#ifdef XT_TLS_DEBUG
		printk("[xt_tls] flow not found");
#endif
		flow = kmalloc(sizeof(flow_data), GFP_KERNEL);
		flow->data = (__u8 *)tcp_header + (tcp_header->doff * 4);
		flow->data_len = skb_tail_pointer(skb) - flow->data;
		flow->hash = hash;
	}

	while (flow->data[header_offset] == 0x16)
	{
#ifdef XT_TLS_DEBUG
		printk("[xt_tls] header_offset: %d", header_offset);
#endif
		Result result;
		tls_header_len = (flow->data[3] << 8) + flow->data[4] + 5;
		handshake_protocol = flow->data[5];

		/*
		 * If we dont have the whole TLS handshake yet,
		 * put the flow_data struct into the hashmap,
		 * and continue when we have more data.
		 */
		if (tls_header_len > flow->data_len)
		{
#ifdef XT_TLS_DEBUG
			printk("[xt_tls] we don't have the whole header yet. store for later.");
#endif
			flow_hashmap_put(&flow_map, &flow->hash, flow);
			return NOT_ENOUGH_DATA;
		}

		if (tls_header_len > 4) {
			// Client Hello
			if (handshake_protocol == 0x1) {
#ifdef XT_TLS_DEBUG
				printk("[xt_tls] found chlo");
#endif
				result = parse_chlo(flow, dest);
			} else
			// Server certificate
			if (handshake_protocol == 0xb) {
#ifdef XT_TLS_DEBUG
				printk("[xt_tls] found server cert");
#endif
				result = parse_server_cert(flow, dest);
			}
		}

		if (result == NAME_FOUND)
		{
			flow_hashmap_remove(&flow_map, &hash);
			kfree(flow->data);
			kfree(flow);
			return result;
		}
		else
		{
			header_offset += tls_header_len;
		}
	}

	flow_hashmap_remove(&flow_map, &hash);
	kfree(flow->data);
	kfree(flow);
	return NAME_NOT_FOUND;
}

static bool tls_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	char *parsed_host;
	const struct xt_tls_info *info = par->matchinfo;
	Result result;
	bool invert = (info->invert & XT_TLS_OP_HOST);
	bool match;

	if ((result = get_tls_hostname(skb, &parsed_host)) != NAME_FOUND)
		return false;

	match = glob_match(info->tls_host, parsed_host);

#ifdef XT_TLS_DEBUG
	printk("[xt_tls] Parsed domain: %s\n", parsed_host);
	printk("[xt_tls] Domain matches: %s, invert: %s\n", match ? "true" : "false", invert ? "true" : "false");
#endif
	if (invert)
		match = !match;

	kfree(parsed_host);

	return match;
}

static int tls_mt_check (const struct xt_mtchk_param *par)
{
	__u16 proto;

	if (par->family == NFPROTO_IPV4) {
		proto = ((const struct ipt_ip *) par->entryinfo)->proto;
	} else if (par->family == NFPROTO_IPV6) {
		proto = ((const struct ip6t_ip6 *) par->entryinfo)->proto;
	} else {
		return -EINVAL;
	}

	if (proto != IPPROTO_TCP) {
		pr_info("Can be used only in combination with "
			"-p tcp\n");
		return -EINVAL;
	}

	return 0;
}

static struct xt_match tls_mt_regs[] __read_mostly = {
	{
		.name       = "tls",
		.revision   = 0,
		.family     = NFPROTO_IPV4,
		.checkentry = tls_mt_check,
		.match      = tls_mt,
		.matchsize  = sizeof(struct xt_tls_info),
		.me         = THIS_MODULE,
	},
#if IS_ENABLED(CONFIG_IP6_NF_IPTABLES)
	{
		.name       = "tls",
		.revision   = 0,
		.family     = NFPROTO_IPV6,
		.checkentry = tls_mt_check,
		.match      = tls_mt,
		.matchsize  = sizeof(struct xt_tls_info),
		.me         = THIS_MODULE,
	},
#endif
};

static int __init tls_mt_init (void)
{
	hashmap_init(&flow_map, hashmap_hash_u32, hashmap_compare_u32, 0);
	return xt_register_matches(tls_mt_regs, ARRAY_SIZE(tls_mt_regs));
}

static void __exit tls_mt_exit (void)
{
	hashmap_destroy(&flow_map);
	xt_unregister_matches(tls_mt_regs, ARRAY_SIZE(tls_mt_regs));
}

module_init(tls_mt_init);
module_exit(tls_mt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nils Andreas Svee <nils@stokkdalen.no>");
MODULE_DESCRIPTION("Xtables: TLS (SNI) matching");
MODULE_ALIAS("ipt_tls");