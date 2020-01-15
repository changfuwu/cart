/* Copyright (C) 2016-2018 Intel Corporation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted for any purpose (including commercial purposes)
 * provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions, and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions, and the following disclaimer in the
 *    documentation and/or materials provided with the distribution.
 *
 * 3. In addition, redistributions of modified forms of the source or binary
 *    code must carry prominent notices stating that the original code was
 *    changed and the date of the change.
 *
 *  4. All publications or advertising materials mentioning features or use of
 *     this software are asked, but not required, to acknowledge that it was
 *     developed by Intel Corporation and credit the contributors.
 *
 * 5. Neither the name of Intel Corporation, nor the name of any Contributor
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * This file is part of CaRT. It implements the main interfaces to mercury.
 */
#define D_LOGFAC	DD_FAC(hg)

#include "crt_internal.h"

/*
 * na_dict table should be in the same order of enum crt_na_type, the last one
 * is terminator with NULL nad_str.
 */
struct crt_na_dict crt_na_dict[] = {
	{
		.nad_type	= CRT_NA_SM,
		.nad_str	= "sm",
		.nad_port_bind	= false,
	}, {
		.nad_type	= CRT_NA_OFI_SOCKETS,
		.nad_str	= "ofi+sockets",
		.nad_port_bind	= true,
	}, {
		.nad_type	= CRT_NA_OFI_VERBS_RXM,
		.nad_str	= "ofi+verbs;ofi_rxm",
		.nad_port_bind	= true,
	}, {
	/* verbs is not supported. Keep entry in order to print warning */
		.nad_type	= CRT_NA_OFI_VERBS,
		.nad_str	= "ofi+verbs",
		.nad_port_bind	= true,
	}, {
		.nad_type	= CRT_NA_OFI_GNI,
		.nad_str	= "ofi+gni",
		.nad_port_bind	= true,
	}, {
		.nad_type	= CRT_NA_OFI_PSM2,
		.nad_str	= "ofi+psm2",
		.nad_port_bind	= true,
	}, {
		.nad_str	= NULL,
	}
};

/**
 * Enable the HG handle pool, can change/tune the max_num and prepost_num.
 * This allows the pool be enabled/re-enabled and be tunable at runtime
 * automatically based on workload or manually by cart_ctl.
 */
static inline int
crt_hg_pool_enable(struct crt_hg_context *hg_ctx, int32_t max_num,
		   int32_t prepost_num)
{
	struct crt_hg_pool	*hg_pool = &hg_ctx->chc_hg_pool;
	struct crt_hg_hdl	*hdl;
	bool			 prepost;
	hg_return_t		 hg_ret = HG_SUCCESS;
	int			 rc = 0;

	if (hg_ctx == NULL || max_num <= 0 || prepost_num < 0 ||
	    prepost_num > max_num) {
		D_ERROR("Invalid parameter of crt_hg_pool_enable, hg_ctx %p, "
			"max_bum %d, prepost_num %d.\n", hg_ctx, max_num,
			prepost_num);
		D_GOTO(out, rc = -DER_INVAL);
	}

	D_SPIN_LOCK(&hg_pool->chp_lock);
	hg_pool->chp_max_num = max_num;
	hg_pool->chp_enabled = true;
	prepost = hg_pool->chp_num < prepost_num;
	D_SPIN_UNLOCK(&hg_pool->chp_lock);

	while (prepost) {
		D_ALLOC_PTR(hdl);
		if (hdl == NULL) {
			rc = -DER_NOMEM;
			break;
		}
		D_INIT_LIST_HEAD(&hdl->chh_link);

		hg_ret = HG_Create(hg_ctx->chc_hgctx, NULL,
				   CRT_HG_RPCID, &hdl->chh_hdl);
		if (hg_ret != HG_SUCCESS) {
			D_FREE_PTR(hdl);
			D_ERROR("HG_Create failed, hg_ret: %d.\n", hg_ret);
			rc = -DER_HG;
			break;
		}

		D_SPIN_LOCK(&hg_pool->chp_lock);
		d_list_add_tail(&hdl->chh_link, &hg_pool->chp_list);
		hg_pool->chp_num++;
		D_DEBUG(DB_NET, "hg_pool %p, add, chp_num %d.\n",
			hg_pool, hg_pool->chp_num);
		if (hg_pool->chp_num >= prepost_num)
			prepost = false;
		D_SPIN_UNLOCK(&hg_pool->chp_lock);
	}

out:
	return rc;
}

static inline void
crt_hg_pool_disable(struct crt_hg_context *hg_ctx)
{
	struct crt_hg_pool	*hg_pool = &hg_ctx->chc_hg_pool;
	struct crt_hg_hdl	*hdl;
	d_list_t		 destroy_list;
	hg_return_t		 hg_ret = HG_SUCCESS;

	D_INIT_LIST_HEAD(&destroy_list);

	D_SPIN_LOCK(&hg_pool->chp_lock);
	hg_pool->chp_num = 0;
	hg_pool->chp_max_num = 0;
	hg_pool->chp_enabled = false;
	d_list_splice_init(&hg_pool->chp_list, &destroy_list);
	D_DEBUG(DB_NET, "hg_pool %p disabled and become empty (chp_num 0).\n",
		hg_pool);
	D_SPIN_UNLOCK(&hg_pool->chp_lock);

	while ((hdl = d_list_pop_entry(&destroy_list,
				       struct crt_hg_hdl,
				       chh_link))) {
		D_ASSERT(hdl->chh_hdl != HG_HANDLE_NULL);
		hg_ret = HG_Destroy(hdl->chh_hdl);
		if (hg_ret != HG_SUCCESS)
			D_ERROR("HG_Destroy failed, hg_hdl %p, hg_ret: %d.\n",
				hdl->chh_hdl, hg_ret);
		else
			D_DEBUG(DB_NET, "hg_hdl %p destroyed.\n", hdl->chh_hdl);
		D_FREE(hdl);
	}
}

static inline int
crt_hg_pool_init(struct crt_hg_context *hg_ctx)
{
	struct crt_hg_pool	*hg_pool = &hg_ctx->chc_hg_pool;
	int			 rc = 0;

	rc = D_SPIN_INIT(&hg_pool->chp_lock, PTHREAD_PROCESS_PRIVATE);
	if (rc != 0)
		D_GOTO(exit, rc);

	hg_pool->chp_num = 0;
	hg_pool->chp_max_num = 0;
	hg_pool->chp_enabled = false;
	D_INIT_LIST_HEAD(&hg_pool->chp_list);

	rc = crt_hg_pool_enable(hg_ctx, CRT_HG_POOL_MAX_NUM,
				CRT_HG_POOL_PREPOST_NUM);
	if (rc != 0)
		D_ERROR("crt_hg_pool_enable, hg_ctx %p, failed rc:%d.\n",
			hg_ctx, rc);
exit:
	return rc;
}

static inline void
crt_hg_pool_fini(struct crt_hg_context *hg_ctx)
{
	struct crt_hg_pool	*hg_pool = &hg_ctx->chc_hg_pool;

	crt_hg_pool_disable(hg_ctx);
	D_SPIN_DESTROY(&hg_pool->chp_lock);
}

static inline struct crt_hg_hdl *
crt_hg_pool_get(struct crt_hg_context *hg_ctx)
{
	struct crt_hg_pool	*hg_pool = &hg_ctx->chc_hg_pool;
	struct crt_hg_hdl	*hdl = NULL;

	D_SPIN_LOCK(&hg_pool->chp_lock);
	if (!hg_pool->chp_enabled) {
		D_DEBUG(DB_NET,
			"hg_pool %p is not enabled cannot get.\n", hg_pool);
		D_GOTO(unlock, hdl);
	}
	hdl = d_list_pop_entry(&hg_pool->chp_list,
			       struct crt_hg_hdl,
			       chh_link);

	if (hdl == NULL) {
		D_DEBUG(DB_NET,
			"hg_pool %p is empty, cannot get.\n", hg_pool);
		D_GOTO(unlock, hdl);
	}

	D_ASSERT(hdl->chh_hdl != HG_HANDLE_NULL);
	hg_pool->chp_num--;
	D_ASSERT(hg_pool->chp_num >= 0);
	D_DEBUG(DB_NET, "hg_pool %p, remove, chp_num %d.\n",
		hg_pool, hg_pool->chp_num);

unlock:
	D_SPIN_UNLOCK(&hg_pool->chp_lock);
	return hdl;
}

/* returns true on success */
static inline bool
crt_hg_pool_put(struct crt_rpc_priv *rpc_priv)
{
	struct crt_context	*ctx = rpc_priv->crp_pub.cr_ctx;
	struct crt_hg_context	*hg_ctx = &ctx->cc_hg_ctx;
	struct crt_hg_pool	*hg_pool = &hg_ctx->chc_hg_pool;
	struct crt_hg_hdl	*hdl;
	bool			 rc = false;

	D_ASSERT(rpc_priv->crp_hg_hdl != HG_HANDLE_NULL);

	if (rpc_priv->crp_hdl_reuse == NULL) {
		D_ALLOC_PTR(hdl);
		if (hdl == NULL)
			D_GOTO(out, 0);
		D_INIT_LIST_HEAD(&hdl->chh_link);
		hdl->chh_hdl = rpc_priv->crp_hg_hdl;
	} else {
		hdl = rpc_priv->crp_hdl_reuse;
		rpc_priv->crp_hdl_reuse = NULL;
	}

	D_SPIN_LOCK(&hg_pool->chp_lock);
	if (hg_pool->chp_enabled && hg_pool->chp_num < hg_pool->chp_max_num) {
		d_list_add_tail(&hdl->chh_link, &hg_pool->chp_list);
		hg_pool->chp_num++;
		D_DEBUG(DB_NET, "hg_pool %p, add, chp_num %d.\n",
			hg_pool, hg_pool->chp_num);
		rc = true;
	} else {
		D_FREE_PTR(hdl);
		D_DEBUG(DB_NET, "hg_pool %p, chp_num %d, max_num %d, "
			"enabled %d, cannot put.\n", hg_pool, hg_pool->chp_num,
			hg_pool->chp_max_num, hg_pool->chp_enabled);
	}
	D_SPIN_UNLOCK(&hg_pool->chp_lock);

out:
	return rc;
}

static hg_return_t
crt_hg_addr_lookup_cb(const struct hg_cb_info *hg_cbinfo)
{
	struct crt_hg_addr_lookup_cb_args	*cb_args = NULL;
	crt_hg_addr_lookup_cb_t			 comp_cb;
	hg_return_t				 rc = HG_SUCCESS;

	cb_args = hg_cbinfo->arg;
	comp_cb = cb_args->al_cb;

	rc = comp_cb(hg_cbinfo->info.lookup.addr, cb_args->al_arg);
	if (rc != 0)
		rc = HG_NA_ERROR;

	D_FREE_PTR(cb_args);

	return rc;
}

/*
 * lookup the NA address of name, fill in the na address in the rpc_priv
 * structure and in the lookup cache of rpc_priv.
 */
int
crt_hg_addr_lookup(struct crt_hg_context *hg_ctx, const char *name,
		   crt_hg_addr_lookup_cb_t complete_cb, void *arg)
{
	struct crt_hg_addr_lookup_cb_args	*cb_args;
	int					 rc = 0;


	D_ALLOC_PTR(cb_args);
	if (cb_args == NULL)
		D_GOTO(out, rc = -DER_NOMEM);

	cb_args->al_cb = complete_cb;
	cb_args->al_arg = arg;
	rc = HG_Addr_lookup(hg_ctx->chc_hgctx, crt_hg_addr_lookup_cb,
			    cb_args, name, HG_OP_ID_IGNORE);
	if (rc != HG_SUCCESS) {
		D_FREE(cb_args);
		D_ERROR("HG_Addr_lookup() failed.\n");
		rc = -DER_HG;
	}

out:
	return rc;
}

int
crt_hg_addr_free(struct crt_hg_context *hg_ctx, hg_addr_t addr)
{
	hg_return_t	ret = HG_SUCCESS;

	ret = HG_Addr_free(hg_ctx->chc_hgcla, addr);
	if (ret != HG_SUCCESS) {
		D_ERROR("HG_Addr_free() failed, hg_ret %d.\n", ret);
		return -DER_HG;
	}

	return 0;
}

int
crt_na_class_get_addr(na_class_t *na_class, char *addr_str, na_size_t *str_size)
{
	na_addr_t	self_addr;
	na_return_t	na_ret;
	int		rc = 0;

	D_ASSERT(na_class != NULL);
	D_ASSERT(addr_str != NULL && str_size != NULL);

	na_ret = NA_Addr_self(na_class, &self_addr);
	if (na_ret != NA_SUCCESS) {
		D_ERROR("NA_Addr_self failed, na_ret: %d.\n", na_ret);
		D_GOTO(out, rc = -DER_HG);
	}

	na_ret = NA_Addr_to_string(na_class, addr_str, str_size, self_addr);
	if (na_ret != NA_SUCCESS) {
		D_ERROR("NA_Addr_to_string failed, na_ret: %d.\n",
			na_ret);
		NA_Addr_free(na_class, self_addr);
		D_GOTO(out, rc = -DER_HG);
	}
	NA_Addr_free(na_class, self_addr);

out:
	return rc;
}

int
crt_hg_get_addr(hg_class_t *hg_class, char *addr_str, size_t *str_size)
{
	hg_addr_t	self_addr;
	hg_return_t	hg_ret;
	int		rc = 0;

	D_ASSERT(hg_class != NULL);
	D_ASSERT(str_size != NULL);

	hg_ret = HG_Addr_self(hg_class, &self_addr);
	if (hg_ret != HG_SUCCESS) {
		D_ERROR("HG_Addr_self failed, hg_ret: %d.\n", hg_ret);
		D_GOTO(out, rc = -DER_HG);
	}

	hg_ret = HG_Addr_to_string(hg_class, addr_str, str_size, self_addr);
	if (hg_ret != HG_SUCCESS) {
		D_ERROR("HG_Addr_to_string failed, hg_ret: %d.\n",
			hg_ret);
		rc = -DER_HG;
	}
	HG_Addr_free(hg_class, self_addr);

out:
	return rc;
}

static int
crt_hg_reg_rpcid(hg_class_t *hg_class)
{
	int rc;

	rc = crt_hg_reg(hg_class, CRT_HG_RPCID,
			(crt_proc_cb_t)crt_proc_in_common,
			(crt_proc_cb_t)crt_proc_out_common,
			(crt_hg_rpc_cb_t)crt_rpc_handler_common);
	if (rc != 0) {
		D_ERROR("crt_hg_reg(rpcid: %#x), failed rc: %d.\n",
			CRT_HG_RPCID, rc);
		D_GOTO(out, rc = -DER_HG);
	}

	rc = crt_hg_reg(hg_class, CRT_HG_ONEWAY_RPCID,
			(crt_proc_cb_t)crt_proc_in_common,
			(crt_proc_cb_t)crt_proc_out_common,
			(crt_hg_rpc_cb_t)crt_rpc_handler_common);
	if (rc != 0) {
		D_ERROR("crt_hg_reg(rpcid: %#x), failed rc: %d.\n",
			CRT_HG_ONEWAY_RPCID, rc);
		D_GOTO(out, rc = -DER_HG);
	}
	rc = HG_Registered_disable_response(hg_class, CRT_HG_ONEWAY_RPCID,
					    HG_TRUE);
	if (rc != 0)
		D_ERROR("HG_Registered_disable_response(rpcid: %#x), "
			"failed rc: %d.\n", CRT_HG_ONEWAY_RPCID, rc);

out:
	return rc;
}

static int
crt_get_info_string(char **string)
{
	int	 port;
	int	 plugin;
	char	*plugin_str;

	plugin = crt_gdata.cg_na_plugin;
	D_ASSERT(plugin == crt_na_dict[plugin].nad_type);
	plugin_str = crt_na_dict[plugin].nad_str;

	if (!crt_na_dict[plugin].nad_port_bind) {
		D_ASPRINTF(*string, "%s://", plugin_str);
	} else if (crt_na_ofi_conf.noc_port == -1) {
		/* OFI_PORT not speicified */

		D_ASPRINTF(*string, "%s://%s/%s", plugin_str,
			crt_na_ofi_conf.noc_domain,
			crt_na_ofi_conf.noc_ip_str);
	} else {
		/* OFI_PORT is only for context 0 to use */
		port = crt_na_ofi_conf.noc_port;
		crt_na_ofi_conf.noc_port = -1;

		D_ASPRINTF(*string, "%s://%s/%s:%d", plugin_str,
			crt_na_ofi_conf.noc_domain,
			crt_na_ofi_conf.noc_ip_str, port);
	}

	if (*string == NULL)
		return -DER_NOMEM;

	return 0;
}

static int
crt_hg_log(FILE *stream, const char *fmt, ...)
{
	va_list		ap;
	int		flags;

	flags = d_log_check((intptr_t)stream);
	if (flags == 0)
		return 0;

	va_start(ap, fmt);
	d_vlog(flags, fmt, ap);
	va_end(ap);

	return 0;
}

/* to be called only in crt_init */
int
crt_hg_init(crt_phy_addr_t *addr, bool server)
{
	char			*info_string = NULL;
	struct crt_hg_gdata	*hg_gdata = NULL;
	na_class_t		*na_class = NULL;
	hg_class_t		*hg_class = NULL;
	struct hg_init_info	 init_info = {};
	int			 rc = 0;

	if (crt_initialized()) {
		D_ERROR("CaRT already initialized.\n");
		D_GOTO(out, rc = -DER_ALREADY);
	}

	/* import HG log */
	hg_log_set_func(crt_hg_log);
	hg_log_set_stream_debug((FILE *)(intptr_t)(D_LOGFAC | DLOG_DBG));
	hg_log_set_stream_warning((FILE *)(intptr_t)(D_LOGFAC | DLOG_WARN));
	hg_log_set_stream_error((FILE *)(intptr_t)(D_LOGFAC | DLOG_ERR));

	D_ASSERTF(*addr == NULL, "Can only be called in crt_init().\n");

	rc = crt_get_info_string(&info_string);
	if (rc != 0)
		D_GOTO(out, rc);

	init_info.na_init_info.progress_mode = NA_DEFAULT;
	init_info.na_init_info.max_contexts = 1;
	if (crt_gdata.cg_share_na == false)
		/* one context per NA class */
		init_info.na_init_info.max_contexts = 1;
	else
		init_info.na_init_info.max_contexts = crt_gdata.cg_ctx_max_num;

	D_DEBUG(DB_NET, "info_string: %s\n", info_string);
	na_class = NA_Initialize_opt(info_string, server,
				     &init_info.na_init_info);
	if (na_class == NULL) {
		D_ERROR("Could not initialize NA class.\n");
		D_GOTO(out, rc = -DER_HG);
	}

	init_info.na_class = na_class;
	/* first two args unused because init_info.na_class is not NULL. */
	hg_class = HG_Init_opt(NULL, server, &init_info);
	if (hg_class == NULL) {
		D_ERROR("Could not initialize HG class.\n");
		NA_Finalize(na_class);
		D_GOTO(out, rc = -DER_HG);
	}

	D_ALLOC_PTR(hg_gdata);
	if (hg_gdata == NULL) {
		HG_Finalize(hg_class);
		NA_Finalize(na_class);
		D_GOTO(out, rc = -DER_NOMEM);
	}

	hg_gdata->chg_nacla = na_class;
	hg_gdata->chg_hgcla = hg_class;

	/* register the shared RPCID */
	rc = crt_hg_reg_rpcid(hg_gdata->chg_hgcla);
	if (rc != 0) {
		D_ERROR("crt_hg_reg_rpcid failed, rc: %d.\n", rc);
		HG_Finalize(hg_class);
		NA_Finalize(na_class);
		D_GOTO(out, rc);
	}

	if (*addr == NULL) {
		char		addr_str[CRT_ADDR_STR_MAX_LEN] = {'\0'};
		na_size_t	str_size = CRT_ADDR_STR_MAX_LEN;

		rc = crt_na_class_get_addr(na_class, addr_str, &str_size);
		if (rc != 0) {
			D_ERROR("crt_na_class_get_addr failed, rc: %d.\n", rc);
			HG_Finalize(hg_class);
			NA_Finalize(na_class);
			D_GOTO(out, rc = -DER_HG);
		}

		D_STRNDUP(*addr, addr_str, CRT_ADDR_STR_MAX_LEN);
		if (*addr == NULL) {
			HG_Finalize(hg_class);
			NA_Finalize(na_class);
			D_GOTO(out, rc = -DER_NOMEM);
		}
	}

	D_DEBUG(DB_NET, "in crt_hg_init, listen address: %s.\n", *addr);
	crt_gdata.cg_hg = hg_gdata;

out:
	if (info_string)
		D_FREE(info_string);

	if (rc != DER_SUCCESS) {
		D_FREE(hg_gdata);
	}
	return rc;
}

/* be called only in crt_finalize */
int
crt_hg_fini()
{
	na_class_t	*na_class;
	hg_class_t	*hg_class;
	hg_return_t	hg_ret;
	na_return_t	na_ret;
	int		rc = DER_SUCCESS;

	na_class = crt_gdata.cg_hg->chg_nacla;
	hg_class = crt_gdata.cg_hg->chg_hgcla;
	D_ASSERT(na_class != NULL);
	D_ASSERT(hg_class != NULL);

	D_DEBUG(DB_NET, "Calling HG_Finalize\n");

	hg_ret = HG_Finalize(hg_class);
	if (hg_ret != HG_SUCCESS) {
		D_WARN("Could not finalize HG class, hg_ret: %d.\n", hg_ret);
		rc = -DER_HG;
	}

	D_DEBUG(DB_NET, "Calling NA_Finalize\n");
	na_ret = NA_Finalize(na_class);
	if (na_ret != NA_SUCCESS) {
		D_WARN("Could not finalize NA class, na_ret: %d.\n", na_ret);
		rc = -DER_HG;
	}

	D_FREE(crt_gdata.cg_hg);
	return rc;
}

int
crt_hg_ctx_init(struct crt_hg_context *hg_ctx, int idx)
{
	struct crt_context	*crt_ctx;
	na_class_t		*na_class = NULL;
	hg_class_t		*hg_class = NULL;
	hg_context_t		*hg_context = NULL;
	char			*info_string = NULL;
	struct hg_init_info	 init_info = {};
	hg_return_t		 hg_ret;
	int			 rc = 0;

	D_ASSERT(hg_ctx != NULL);
	crt_ctx = container_of(hg_ctx, struct crt_context, cc_hg_ctx);

	D_DEBUG(DB_NET, "crt_gdata.cg_share_na %d, crt_is_service() %d\n",
			crt_gdata.cg_share_na, crt_is_service());
	if (idx == 0 || crt_gdata.cg_share_na == true) {
		hg_context = HG_Context_create_id(crt_gdata.cg_hg->chg_hgcla,
						  idx);
		if (hg_context == NULL) {
			D_ERROR("Could not create HG context.\n");
			D_GOTO(out, rc = -DER_HG);
		}

		/* register crt_ctx to get it in crt_rpc_handler_common */
		hg_ret = HG_Context_set_data(hg_context, crt_ctx, NULL);
		if (hg_ret != HG_SUCCESS) {
			D_ERROR("HG_Context_set_data failed, ret: %d.\n",
				hg_ret);
			HG_Context_destroy(hg_context);
			D_GOTO(out, rc = -DER_HG);
		}

		hg_ctx->chc_nacla = crt_gdata.cg_hg->chg_nacla;
		hg_ctx->chc_hgcla = crt_gdata.cg_hg->chg_hgcla;
		D_DEBUG(DB_NET, "hg_ctx->chc_hgcla %p\n", hg_ctx->chc_hgcla);
		hg_ctx->chc_shared_na = true;
	} else {
		char		addr_str[CRT_ADDR_STR_MAX_LEN] = {'\0'};
		na_size_t	str_size = CRT_ADDR_STR_MAX_LEN;

		rc = crt_get_info_string(&info_string);
		if (rc != 0)
			D_GOTO(out, rc);

		init_info.na_init_info.progress_mode = NA_DEFAULT;
		init_info.na_init_info.max_contexts = 1;
		na_class = NA_Initialize_opt(info_string, crt_is_service(),
					     &init_info.na_init_info);
		if (na_class == NULL) {
			D_ERROR("Could not initialize NA class.\n");
			D_GOTO(out, rc = -DER_HG);
		}

		rc = crt_na_class_get_addr(na_class, addr_str, &str_size);
		if (rc != 0) {
			D_ERROR("crt_na_class_get_addr failed, rc: %d.\n", rc);
			NA_Finalize(na_class);
			D_GOTO(out, rc = -DER_HG);
		}
		D_DEBUG(DB_NET, "New context(idx:%d), listen address: %s.\n",
			idx, addr_str);

		init_info.na_class = na_class;
		/* first two args unused because init_info.na_class is not NULL.
		 */
		hg_class = HG_Init_opt(NULL, false, &init_info);
		if (hg_class == NULL) {
			D_ERROR("Could not initialize HG class.\n");
			NA_Finalize(na_class);
			D_GOTO(out, rc = -DER_HG);
		}

		hg_context = HG_Context_create(hg_class);
		if (hg_context == NULL) {
			D_ERROR("Could not create HG context.\n");
			HG_Finalize(hg_class);
			NA_Finalize(na_class);
			D_GOTO(out, rc = -DER_HG);
		}

		/* register the shared RPCID to every hg_class */
		rc = crt_hg_reg_rpcid(hg_class);
		if (rc != 0) {
			D_ERROR("crt_hg_reg_rpcid failed, rc: %d.\n", rc);
			HG_Context_destroy(hg_context);
			HG_Finalize(hg_class);
			NA_Finalize(na_class);
			D_GOTO(out, rc);
		}

		D_DEBUG(DB_NET, "crt_gdata.cg_hg->chg_hgcla %p\n",
			crt_gdata.cg_hg->chg_hgcla);
		/* register crt_ctx to get it in crt_rpc_handler_common */
		hg_ret = HG_Context_set_data(hg_context, crt_ctx, NULL);
		if (hg_ret != HG_SUCCESS) {
			D_ERROR("HG_Context_set_data failed, ret: %d.\n",
				hg_ret);
			HG_Context_destroy(hg_context);
			HG_Finalize(hg_class);
			NA_Finalize(na_class);
			D_GOTO(out, rc = -DER_HG);
		}

		hg_ctx->chc_nacla = na_class;
		hg_ctx->chc_hgcla = hg_class;
		hg_ctx->chc_shared_na = false;
	}

	hg_ctx->chc_hgctx = hg_context;
	/* TODO: need to create separate bulk class and bulk context? */
	hg_ctx->chc_bulkcla = hg_ctx->chc_hgcla;
	hg_ctx->chc_bulkctx = hg_ctx->chc_hgctx;
	D_ASSERT(hg_ctx->chc_bulkcla != NULL);
	D_ASSERT(hg_ctx->chc_bulkctx != NULL);

	rc = crt_hg_pool_init(hg_ctx);
	if (rc != 0)
		D_ERROR("context idx %d hg_ctx %p, crt_hg_pool_init failed, "
			"rc: %d.\n", idx, hg_ctx, rc);

out:
	if (info_string)
		D_FREE(info_string);
	return rc;
}

int
crt_hg_ctx_fini(struct crt_hg_context *hg_ctx)
{
	hg_context_t	*hg_context;
	hg_return_t	hg_ret = HG_SUCCESS;
	na_return_t	na_ret;
	int		rc = 0;

	D_ASSERT(hg_ctx != NULL);
	hg_context = hg_ctx->chc_hgctx;
	D_ASSERT(hg_context != NULL);

	crt_hg_pool_fini(hg_ctx);

	hg_ret = HG_Context_destroy(hg_context);
	if (hg_ret == HG_SUCCESS) {
		hg_ctx->chc_hgctx = NULL;
	} else {
		D_ERROR("Could not destroy HG context, hg_ret: %d.\n", hg_ret);
		D_GOTO(out, rc = -DER_HG);
	}

	if (hg_ctx->chc_shared_na == true)
		goto out;

	/* the hg_context destroyed, ignore below errors with warn msg */
	hg_ret = HG_Finalize(hg_ctx->chc_hgcla);
	if (hg_ret != HG_SUCCESS)
		D_WARN("Could not finalize HG class, hg_ret: %d.\n", hg_ret);

	na_ret = NA_Finalize(hg_ctx->chc_nacla);
	if (na_ret != NA_SUCCESS)
		D_WARN("Could not finalize NA class, na_ret: %d.\n", na_ret);

out:
	return rc;
}

struct crt_context *
crt_hg_context_lookup(hg_context_t *hg_ctx)
{
	struct crt_context	*crt_ctx;
	int			found = 0;

	D_RWLOCK_RDLOCK(&crt_gdata.cg_rwlock);

	d_list_for_each_entry(crt_ctx, &crt_gdata.cg_ctx_list, cc_link) {
		if (crt_ctx->cc_hg_ctx.chc_hgctx == hg_ctx) {
			found = 1;
			break;
		}
	}

	D_RWLOCK_UNLOCK(&crt_gdata.cg_rwlock);

	return (found == 1) ? crt_ctx : NULL;
}

int
crt_rpc_handler_common(hg_handle_t hg_hdl)
{
	struct crt_context	*crt_ctx;
	struct crt_hg_context	*hg_ctx;
	const struct hg_info	*hg_info;
	struct crt_rpc_priv	*rpc_priv;
	crt_rpc_t		*rpc_pub;
	crt_opcode_t		 opc;
	crt_proc_t		 proc = NULL;
	struct crt_opc_info	*opc_info = NULL;
	hg_return_t		 hg_ret = HG_SUCCESS;
	bool			 is_coll_req = false;
	int			 rc = 0;
	struct crt_rpc_priv	 rpc_tmp = {0};

	hg_info = HG_Get_info(hg_hdl);
	if (hg_info == NULL) {
		D_ERROR("HG_Get_info failed.\n");
		D_GOTO(out, hg_ret = HG_PROTOCOL_ERROR);
	}

	crt_ctx = (struct crt_context *)HG_Context_get_data(
			hg_info->context);
	if (crt_ctx == NULL) {
		D_ERROR("HG_Context_get_data failed.\n");
		D_GOTO(out, hg_ret = HG_PROTOCOL_ERROR);
	}
	hg_ctx = &crt_ctx->cc_hg_ctx;
	D_ASSERT(hg_ctx->chc_hgcla == hg_info->hg_class);
	D_ASSERT(hg_ctx->chc_hgctx == hg_info->context);

	rpc_tmp.crp_hg_addr = hg_info->addr;
	rpc_tmp.crp_hg_hdl = hg_hdl;
	rpc_tmp.crp_pub.cr_ctx = crt_ctx;

	rc = crt_hg_unpack_header(hg_hdl, &rpc_tmp, &proc);
	if (rc != 0) {
		D_ERROR("crt_hg_unpack_header failed, rc: %d.\n", rc);
		crt_hg_reply_error_send(&rpc_tmp, -DER_MISC);
		/** safe to return here because relevant portion of rpc_tmp is
		 * already serialized by Mercury. Same for below.
		 */
		HG_Destroy(rpc_tmp.crp_hg_hdl);
		D_GOTO(out, hg_ret = HG_SUCCESS);
	}
	D_ASSERT(proc != NULL);
	opc = rpc_tmp.crp_req_hdr.cch_opc;

	/**
	 * Set the opcode in the temp RPC so that it can be correctly logged.
	 */
	rpc_tmp.crp_pub.cr_opc = opc;

	opc_info = crt_opc_lookup(crt_gdata.cg_opc_map, opc, CRT_UNLOCK);
	if (opc_info == NULL) {
		D_ERROR("opc: %#x, lookup failed.\n", opc);
		/*
		 * The RPC is not registered on the server, we don't know how to
		 * process the RPC request, so we send a CART
		 * level error message to the client.
		 */
		crt_hg_reply_error_send(&rpc_tmp, -DER_UNREG);
		crt_hg_unpack_cleanup(proc);

		HG_Destroy(rpc_tmp.crp_hg_hdl);
		D_GOTO(out, hg_ret = HG_SUCCESS);
	}
	D_ASSERT(opc_info->coi_opc == opc);

	D_ALLOC(rpc_priv, opc_info->coi_rpc_size);
	if (rpc_priv == NULL) {
		crt_hg_reply_error_send(&rpc_tmp, -DER_DOS);
		crt_hg_unpack_cleanup(proc);
		HG_Destroy(rpc_tmp.crp_hg_hdl);
		D_GOTO(out, hg_ret = HG_SUCCESS);
	}
	crt_hg_header_copy(&rpc_tmp, rpc_priv);
	rpc_pub = &rpc_priv->crp_pub;

	if (rpc_priv->crp_flags & CRT_RPC_FLAG_COLL) {
		is_coll_req = true;
		rpc_priv->crp_input_got = 1;
	}

	rpc_priv->crp_opc_info = opc_info;
	rpc_pub->cr_opc = rpc_tmp.crp_pub.cr_opc;
	rpc_pub->cr_ep.ep_rank = rpc_priv->crp_req_hdr.cch_dst_rank;
	rpc_pub->cr_ep.ep_tag = rpc_priv->crp_req_hdr.cch_dst_tag;

	RPC_TRACE(DB_TRACE, rpc_priv,
		  "(opc: %#x rpc_pub: %p) allocated per RPC request received.\n",
		  rpc_priv->crp_opc_info->coi_opc,
		  &rpc_priv->crp_pub);

	rc = crt_rpc_priv_init(rpc_priv, crt_ctx, true /* srv_flag */);
	if (rc != 0) {
		D_ERROR("crt_rpc_priv_init rc=%d, opc=%#x\n", rc, opc);
		crt_hg_reply_error_send(rpc_priv, -DER_MISC);
		HG_Destroy(rpc_tmp.crp_hg_hdl);
		D_GOTO(out, hg_ret = HG_SUCCESS);
	}

	D_ASSERT(rpc_priv->crp_srv != 0);
	if (rpc_pub->cr_input_size > 0) {
		D_ASSERT(rpc_pub->cr_input != NULL);
		D_ASSERT(opc_info->coi_crf != NULL);
		D_ASSERT(opc_info->coi_crf->crf_size_in == rpc_pub->cr_input_size);
		/* corresponding to HG_Free_input in crt_hg_req_destroy */
		rc = crt_hg_unpack_body(rpc_priv, proc);
		if (rc == 0) {
			rpc_priv->crp_input_got = 1;
			rpc_pub->cr_ep.ep_grp = NULL;
			/* TODO lookup by rpc_priv->crp_req_hdr.cch_grp_id */
		} else {
			D_ERROR("_unpack_body failed, rc: %d, opc: %#x.\n",
				rc, rpc_pub->cr_opc);
			crt_hg_reply_error_send(rpc_priv, -DER_MISC);
			D_GOTO(decref, hg_ret = HG_SUCCESS);
		}
	} else {
		crt_hg_unpack_cleanup(proc);
	}

	if (opc_info->coi_rpc_cb == NULL) {
		D_ERROR("NULL crp_hg_hdl, opc: %#x.\n", opc);
		crt_hg_reply_error_send(rpc_priv, -DER_UNREG);
		D_GOTO(decref, hg_ret = HG_SUCCESS);
	}

	if (!is_coll_req)
		rc = crt_rpc_common_hdlr(rpc_priv);
	else
		rc = crt_corpc_common_hdlr(rpc_priv);
	if (rc != 0) {
		RPC_ERROR(rpc_priv,
			  "failed to invoke RPC handler, rc: %d, opc: %#x\n",
			  rc, opc);
		crt_hg_reply_error_send(rpc_priv, rc);
	}

decref:
	/* If rpc call back is customized, then it might be handled
	 * asynchronously, Let's hold the RPC, and the real handler
	 * (crt_handle_rpc())will release it
	 */
	if (rc != 0 || !crt_rpc_cb_customized(crt_ctx, &rpc_priv->crp_pub))
		RPC_DECREF(rpc_priv);
out:
	return hg_ret;
}

int
crt_hg_req_create(struct crt_hg_context *hg_ctx, struct crt_rpc_priv *rpc_priv)
{
	hg_id_t		rpcid;
	hg_return_t	hg_ret = HG_SUCCESS;
	bool		hg_created = false;
	int		rc = 0;

	D_ASSERT(hg_ctx != NULL && hg_ctx->chc_hgcla != NULL &&
		 hg_ctx->chc_hgctx != NULL);
	D_ASSERT(rpc_priv != NULL);
	D_ASSERT(rpc_priv->crp_opc_info != NULL);

	if (!rpc_priv->crp_opc_info->coi_no_reply) {
		rpcid = CRT_HG_RPCID;
		rpc_priv->crp_hdl_reuse = crt_hg_pool_get(hg_ctx);
	} else {
		rpcid = CRT_HG_ONEWAY_RPCID;
	}

	if (rpc_priv->crp_hdl_reuse == NULL) {
		hg_ret = HG_Create(hg_ctx->chc_hgctx, rpc_priv->crp_hg_addr,
				   rpcid, &rpc_priv->crp_hg_hdl);
		if (hg_ret == HG_SUCCESS) {
			hg_created = true;
		} else {
			RPC_ERROR(rpc_priv,
				  "HG_Create failed, hg_ret: %d\n",
				  hg_ret);
			rc = -DER_HG;
		}
	} else {
		rpc_priv->crp_hg_hdl = rpc_priv->crp_hdl_reuse->chh_hdl;
		hg_ret = HG_Reset(rpc_priv->crp_hg_hdl, rpc_priv->crp_hg_addr,
				  0 /* reuse original rpcid */);
		if (hg_ret != HG_SUCCESS) {
			rpc_priv->crp_hg_hdl = NULL;
			RPC_ERROR(rpc_priv,
				  "HG_Reset failed, hg_ret: %d\n",
				  hg_ret);
			rc = -DER_HG;
		}
	}
	if (crt_gdata.cg_share_na == true) {
		hg_ret = HG_Set_target_id(rpc_priv->crp_hg_hdl,
					  rpc_priv->crp_pub.cr_ep.ep_tag);
		if (hg_ret != HG_SUCCESS) {
			if (hg_created)
				HG_Destroy(rpc_priv->crp_hg_hdl);
			RPC_ERROR(rpc_priv,
				  "HG_Set_target_id failed, hg_ret: %d\n",
				  hg_ret);
			rc = -DER_HG;
		}
	}

	return rc;
}

void
crt_hg_req_destroy(struct crt_rpc_priv *rpc_priv)
{
	hg_return_t hg_ret;

	D_ASSERT(rpc_priv != NULL);
	if (rpc_priv->crp_output_got != 0) {
		hg_ret = HG_Free_output(rpc_priv->crp_hg_hdl,
					&rpc_priv->crp_pub.cr_output);
		if (hg_ret != HG_SUCCESS) {
			RPC_ERROR(rpc_priv,
				  "HG_Free_output failed, hg_ret: %d\n",
				  hg_ret);
		}
	}
	if (rpc_priv->crp_input_got != 0) {
		hg_ret = HG_Free_input(rpc_priv->crp_hg_hdl,
				       &rpc_priv->crp_pub.cr_input);
		if (hg_ret != HG_SUCCESS)
			RPC_ERROR(rpc_priv,
				  "HG_Free_input failed, hg_ret: %d\n",
				  hg_ret);
	}

	crt_rpc_priv_fini(rpc_priv);

	if (!rpc_priv->crp_coll && rpc_priv->crp_hg_hdl != NULL &&
		(rpc_priv->crp_input_got == 0)) {
		if (!rpc_priv->crp_srv &&
		    !rpc_priv->crp_opc_info->coi_no_reply) {

			if (crt_hg_pool_put(rpc_priv)) {
				RPC_TRACE(DB_NET, rpc_priv,
					  "hg_hdl %p put to pool.\n",
					  rpc_priv->crp_hg_hdl);
				D_GOTO(mem_free, 0);
			}
		}
		/* HACK alert:  Do we need to provide a low-level interface
		 * for HG_Free_input since we do low level packing.   Without
		 * calling HG_Get_input, we don't take a reference on the
		 * handle calling destroy here can result in the handle
		 * getting freed before mercury is done with it
		 */
		hg_ret = HG_Destroy(rpc_priv->crp_hg_hdl);
		if (hg_ret != HG_SUCCESS) {
			RPC_ERROR(rpc_priv, "HG_Destroy failed, hg_ret: %d\n",
				  hg_ret);
		}
	}

mem_free:

	RPC_TRACE(DB_TRACE, rpc_priv, "destroying\n");

	crt_rpc_priv_free(rpc_priv);
}

/* the common completion callback for sending RPC request */
static hg_return_t
crt_hg_req_send_cb(const struct hg_cb_info *hg_cbinfo)
{
	struct crt_cb_info	crt_cbinfo;
	crt_rpc_t		*rpc_pub;
	struct crt_rpc_priv	*rpc_priv = hg_cbinfo->arg;
	hg_return_t		hg_ret = HG_SUCCESS;
	crt_rpc_state_t		state;
	int			rc = 0;

	D_ASSERT(rpc_priv != NULL);
	D_ASSERT(hg_cbinfo->type == HG_CB_FORWARD);

	rpc_pub = &rpc_priv->crp_pub;

	RPC_TRACE(DB_TRACE, rpc_priv, "entered, hg_cbinfo->ret %d.\n",
		  hg_cbinfo->ret);
	switch (hg_cbinfo->ret) {
	case HG_SUCCESS:
		state = RPC_STATE_COMPLETED;
		break;
	case HG_CANCELED:
		if (!CRT_RANK_PRESENT(rpc_pub->cr_ep.ep_grp,
				     rpc_pub->cr_ep.ep_rank)) {
			RPC_TRACE(DB_NET, rpc_priv, "request target evicted\n");
			rc = -DER_EVICTED;
		} else if (crt_req_timedout(rpc_priv)) {
			RPC_TRACE(DB_NET, rpc_priv, "request timedout\n");
			rc = -DER_TIMEDOUT;
		} else {
			RPC_TRACE(DB_NET, rpc_priv, "request canceled\n");
			rc = -DER_CANCELED;
		}
		state = RPC_STATE_CANCELED;
		rpc_priv->crp_state = state;
		hg_ret = hg_cbinfo->ret;
		break;
	default:
		state = RPC_STATE_COMPLETED;
		rc = -DER_HG;
		hg_ret = hg_cbinfo->ret;
		RPC_TRACE(DB_NET, rpc_priv,
			  "hg_cbinfo->ret: %d.\n", hg_cbinfo->ret);
		break;
	}

	if (rpc_priv->crp_complete_cb == NULL) {
		rpc_priv->crp_state = state;
		D_GOTO(out, hg_ret);
	}

	if (rc == 0) {
		rpc_priv->crp_state = RPC_STATE_REPLY_RECVED;
		if (rpc_priv->crp_opc_info->coi_no_reply == 0) {
			/* HG_Free_output in crt_hg_req_destroy */
			hg_ret = HG_Get_output(hg_cbinfo->info.forward.handle,
					       &rpc_pub->cr_output);
			if (hg_ret == HG_SUCCESS) {
				rpc_priv->crp_output_got = 1;
				rc = rpc_priv->crp_reply_hdr.cch_rc;
			} else {
				RPC_ERROR(rpc_priv,
					  "HG_Get_output failed, hg_ret: %d\n",
					  hg_ret);
				rc = -DER_HG;
			}
		}
	}

	crt_cbinfo.cci_rpc = rpc_pub;
	crt_cbinfo.cci_arg = rpc_priv->crp_arg;
	crt_cbinfo.cci_rc = rc;

	if (crt_cbinfo.cci_rc != 0)
		RPC_ERROR(rpc_priv, "RPC failed; rc: %d\n",
			  crt_cbinfo.cci_rc);

	RPC_TRACE(DB_TRACE, rpc_priv,
		  "Invoking RPC callback (rank %d tag %d) rc: %d.\n",
		  rpc_priv->crp_pub.cr_ep.ep_rank,
		  rpc_priv->crp_pub.cr_ep.ep_tag,
		  crt_cbinfo.cci_rc);

	rpc_priv->crp_complete_cb(&crt_cbinfo);

	rpc_priv->crp_state = state;

out:
	crt_context_req_untrack(rpc_priv);

	/* corresponding to the refcount taken in crt_rpc_priv_init(). */
	RPC_DECREF(rpc_priv);

	return hg_ret;
}

int
crt_hg_req_send(struct crt_rpc_priv *rpc_priv)
{
	hg_return_t	 hg_ret;
	int		 rc = DER_SUCCESS;

	D_ASSERT(rpc_priv != NULL);

	/* take a ref ahead to make sure rpc_priv be valid even if timeout
	 * happen before HG_Forward returns (it is possible due to blocking
	 * in socket provider now).
	 */
	RPC_ADDREF(rpc_priv);

	hg_ret = HG_Forward(rpc_priv->crp_hg_hdl, crt_hg_req_send_cb, rpc_priv,
			    &rpc_priv->crp_pub.cr_input);
	if (hg_ret != HG_SUCCESS) {
		RPC_ERROR(rpc_priv,
			  "HG_Forward failed, hg_ret: %d\n",
			  hg_ret);
	} else {
		RPC_TRACE(DB_TRACE, rpc_priv,
			  "sent to rank %d uri: %s\n",
			  rpc_priv->crp_pub.cr_ep.ep_rank,
			  rpc_priv->crp_tgt_uri);
	}

	if (hg_ret == HG_NA_ERROR) {
		if (!crt_req_timedout(rpc_priv)) {
			/* error will be reported to the completion callback in
			 * crt_req_timeout_hdlr()
			 */
			crt_req_force_timeout(rpc_priv);
		}
		rpc_priv->crp_state = RPC_STATE_FWD_UNREACH;
	} else if (hg_ret != HG_SUCCESS) {
		rc = -DER_HG;
	} else {
		rpc_priv->crp_on_wire = 1;
	}

	RPC_DECREF(rpc_priv);

	return rc;
}

int
crt_hg_req_cancel(struct crt_rpc_priv *rpc_priv)
{
	hg_return_t	hg_ret;
	int		rc = 0;

	D_ASSERT(rpc_priv != NULL);
	if (!rpc_priv->crp_hg_hdl)
		D_GOTO(out, rc = -DER_INVAL);

	hg_ret = HG_Cancel(rpc_priv->crp_hg_hdl);
	if (hg_ret != HG_SUCCESS) {
		RPC_ERROR(rpc_priv, "crt_hg_req_cancel failed, hg_ret: %d\n",
			  hg_ret);
		rc = -DER_HG;
	}

out:
	return rc;
}

/* just to release the reference taken at crt_hg_reply_send */
static hg_return_t
crt_hg_reply_send_cb(const struct hg_cb_info *hg_cbinfo)
{
	struct crt_rpc_priv	*rpc_priv = hg_cbinfo->arg;
	hg_return_t		hg_ret;
	crt_opcode_t		opc;

	D_ASSERT(rpc_priv != NULL);

	opc = rpc_priv->crp_pub.cr_opc;
	hg_ret = hg_cbinfo->ret;
	/* Check for the return code here but it's not automatically an error,
	 * see CART-146 for details
	 */
	if (hg_ret != HG_SUCCESS)
		D_WARN("hg_cbinfo->ret: %d, opc: %#x.\n", hg_ret, opc);

	/* corresponding to the crt_req_addref in crt_hg_reply_send */
	RPC_DECREF(rpc_priv);

	return hg_ret;
}

int
crt_hg_reply_send(struct crt_rpc_priv *rpc_priv)
{
	hg_return_t	hg_ret;
	int		rc = 0;

	D_ASSERT(rpc_priv != NULL);

	RPC_ADDREF(rpc_priv);
	hg_ret = HG_Respond(rpc_priv->crp_hg_hdl, crt_hg_reply_send_cb,
			    rpc_priv, &rpc_priv->crp_pub.cr_output);
	if (hg_ret != HG_SUCCESS) {
		RPC_ERROR(rpc_priv,
			  "HG_Respond failed, hg_ret: %d\n",
			  hg_ret);
		/* should success as addref above */
		RPC_DECREF(rpc_priv);
		rc = (hg_ret == HG_PROTOCOL_ERROR) ? -DER_PROTO : -DER_HG;
	}

	return rc;
}

void
crt_hg_reply_error_send(struct crt_rpc_priv *rpc_priv, int error_code)
{
	void	*hg_out_struct;
	int	 hg_ret;


	D_ASSERT(rpc_priv != NULL);
	D_ASSERT(error_code != 0);

	hg_out_struct = &rpc_priv->crp_pub.cr_output;
	rpc_priv->crp_reply_hdr.cch_rc = error_code;
	hg_ret = HG_Respond(rpc_priv->crp_hg_hdl, NULL, NULL, hg_out_struct);
	if (hg_ret != HG_SUCCESS) {
		RPC_ERROR(rpc_priv,
			  "Failed to send CART error code back. HG_Respond failed, hg_ret: %d\n",
			  hg_ret);
	} else {
		RPC_TRACE(DB_NET, rpc_priv,
			  "Sent CART level error message back to client. error_code: %d\n",
			  error_code);
	}
	rpc_priv->crp_reply_pending = 0;
}

static int
crt_hg_trigger(struct crt_hg_context *hg_ctx)
{
	hg_context_t		*hg_context;
	hg_return_t		hg_ret = HG_SUCCESS;
	unsigned int		count = 0;

	D_ASSERT(hg_ctx != NULL);
	hg_context = hg_ctx->chc_hgctx;

	do {
		hg_ret = HG_Trigger(hg_context, 0, UINT32_MAX, &count);
	} while (hg_ret == HG_SUCCESS && count > 0);

	if (hg_ret != HG_TIMEOUT) {
		D_ERROR("HG_Trigger failed, hg_ret: %d.\n", hg_ret);
		return -DER_HG;
	}

	return 0;
}

int
crt_hg_progress(struct crt_hg_context *hg_ctx, int64_t timeout)
{
	hg_context_t		*hg_context;
	hg_class_t		*hg_class;
	hg_return_t		hg_ret = HG_SUCCESS;
	unsigned int		hg_timeout;
	int			rc;

	D_ASSERT(hg_ctx != NULL);
	hg_context = hg_ctx->chc_hgctx;
	hg_class = hg_ctx->chc_hgcla;
	D_ASSERT(hg_context != NULL && hg_class != NULL);

	/**
	 * Mercury only supports milli-second timeout and uses an unsigned int
	 */
	if (timeout < 0)
		hg_timeout = UINT32_MAX;
	else
		hg_timeout = timeout / 1000;

	rc = crt_hg_trigger(hg_ctx);
	if (rc != 0)
		return rc;

	/** progress RPC execution */
	hg_ret = HG_Progress(hg_context, hg_timeout);
	if (hg_ret == HG_TIMEOUT)
		D_GOTO(out, rc = -DER_TIMEDOUT);
	if (hg_ret != HG_SUCCESS) {
		D_ERROR("HG_Progress failed, hg_ret: %d.\n", hg_ret);
		D_GOTO(out, rc = -DER_HG);
	}

	/* some RPCs have progressed, call Trigger again */
	rc = crt_hg_trigger(hg_ctx);

out:
	return rc;
}

#define CRT_HG_IOVN_STACK	(8)
int
crt_hg_bulk_create(struct crt_hg_context *hg_ctx, d_sg_list_t *sgl,
		   crt_bulk_perm_t bulk_perm, crt_bulk_t *bulk_hdl)
{
	void		**buf_ptrs = NULL;
	void		*buf_ptrs_stack[CRT_HG_IOVN_STACK];
	hg_size_t	*buf_sizes = NULL;
	hg_size_t	buf_sizes_stack[CRT_HG_IOVN_STACK];
	hg_uint8_t	flags;
	hg_bulk_t	hg_bulk_hdl;
	hg_return_t	hg_ret = HG_SUCCESS;
	int		rc = 0, i;
	bool		allocate = false;

	D_ASSERT(hg_ctx != NULL && hg_ctx->chc_bulkcla != NULL);
	D_ASSERT(sgl != NULL && bulk_hdl != NULL);
	D_ASSERT(bulk_perm == CRT_BULK_RW || bulk_perm == CRT_BULK_RO);

	flags = (bulk_perm == CRT_BULK_RW) ? HG_BULK_READWRITE :
					     HG_BULK_READ_ONLY;

	if (sgl->sg_nr <= CRT_HG_IOVN_STACK) {
		buf_sizes = buf_sizes_stack;
	} else {
		allocate = true;
		D_ALLOC_ARRAY(buf_sizes, sgl->sg_nr);
		if (buf_sizes == NULL)
			D_GOTO(out, rc = -DER_NOMEM);
	}
	for (i = 0; i < sgl->sg_nr; i++)
		buf_sizes[i] = sgl->sg_iovs[i].iov_buf_len;

	if (sgl->sg_iovs == NULL) {
		buf_ptrs = NULL;
	} else {
		if (allocate) {
			D_ALLOC_ARRAY(buf_ptrs, sgl->sg_nr);
			if (buf_ptrs == NULL)
				D_GOTO(out, rc = -DER_NOMEM);
		} else {
			buf_ptrs = buf_ptrs_stack;
		}

		for (i = 0; i < sgl->sg_nr; i++)
			buf_ptrs[i] = sgl->sg_iovs[i].iov_buf;
	}

	hg_ret = HG_Bulk_create(hg_ctx->chc_bulkcla, sgl->sg_nr, buf_ptrs,
				buf_sizes, flags, &hg_bulk_hdl);
	if (hg_ret == HG_SUCCESS) {
		*bulk_hdl = hg_bulk_hdl;
	} else {
		D_ERROR("HG_Bulk_create failed, hg_ret: %d.\n", hg_ret);
		rc = -DER_HG;
	}

out:
	/* HG_Bulk_create copied the parameters, can free here */
	if (allocate) {
		D_FREE(buf_ptrs);
		D_FREE(buf_sizes);
	}

	return rc;
}

int
crt_hg_bulk_bind(crt_bulk_t bulk_hdl, struct crt_hg_context *hg_ctx)
{
	hg_return_t	  hg_ret = HG_SUCCESS;

	hg_ret = HG_Bulk_bind(bulk_hdl, hg_ctx->chc_hgctx);
	if (hg_ret != HG_SUCCESS)
		D_ERROR("HG_Bulk_bind failed, hg_ret %d.\n", hg_ret);

	return crt_hgret_2_der(hg_ret);
}

int
crt_hg_bulk_access(crt_bulk_t bulk_hdl, d_sg_list_t *sgl)
{
	unsigned int	  bulk_sgnum;
	unsigned int	  actual_sgnum;
	size_t		  bulk_len;
	void		**buf_ptrs = NULL;
	void		 *buf_ptrs_stack[CRT_HG_IOVN_STACK];
	hg_size_t	 *buf_sizes = NULL;
	hg_size_t	  buf_sizes_stack[CRT_HG_IOVN_STACK];
	hg_bulk_t	  hg_bulk_hdl;
	hg_return_t	  hg_ret = HG_SUCCESS;
	int		  rc = 0, i;
	bool		  allocate = false;

	D_ASSERT(bulk_hdl != CRT_BULK_NULL && sgl != NULL);

	rc = crt_bulk_get_sgnum(bulk_hdl, &bulk_sgnum);
	if (rc != 0) {
		D_ERROR("crt_bulk_get_sgnum failed, rc: %d.\n", rc);
		D_GOTO(out, rc);
	}
	rc = crt_bulk_get_len(bulk_hdl, &bulk_len);
	if (rc != 0) {
		D_ERROR("crt_bulk_get_len failed, rc: %d.\n", rc);
		D_GOTO(out, rc);
	}

	if (sgl->sg_nr < bulk_sgnum) {
		D_DEBUG(DB_NET, "sgl->sg_nr (%d) too small, %d required.\n",
			sgl->sg_nr, bulk_sgnum);
		sgl->sg_nr_out = bulk_sgnum;
		D_GOTO(out, rc = -DER_TRUNC);
	}

	if (bulk_sgnum <= CRT_HG_IOVN_STACK) {
		buf_sizes = buf_sizes_stack;
		buf_ptrs = buf_ptrs_stack;
	} else {
		D_ALLOC_ARRAY(buf_sizes, bulk_sgnum);
		if (buf_sizes == NULL)
			D_GOTO(out, rc = -DER_NOMEM);

		D_ALLOC_ARRAY(buf_ptrs, bulk_sgnum);
		if (buf_ptrs == NULL) {
			D_FREE(buf_sizes);
			D_GOTO(out, rc = -DER_NOMEM);
		}
		allocate = true;
	}

	hg_bulk_hdl = bulk_hdl;
	hg_ret = HG_Bulk_access(hg_bulk_hdl, 0, bulk_len, HG_BULK_READWRITE,
				bulk_sgnum, buf_ptrs, buf_sizes, &actual_sgnum);
	if (hg_ret != HG_SUCCESS) {
		D_ERROR("HG_Bulk_access failed, hg_ret: %d.\n", hg_ret);
		D_GOTO(out, rc = -DER_HG);
	}
	D_ASSERT(actual_sgnum == bulk_sgnum);

	for (i = 0; i < bulk_sgnum; i++) {
		sgl->sg_iovs[i].iov_buf = buf_ptrs[i];
		sgl->sg_iovs[i].iov_buf_len = buf_sizes[i];
		sgl->sg_iovs[i].iov_len = buf_sizes[i];
	}
	sgl->sg_nr_out = bulk_sgnum;

out:
	if (allocate) {
		D_FREE(buf_sizes);
		D_FREE(buf_ptrs);
	}
	return rc;
}

struct crt_hg_bulk_cbinfo {
	struct crt_bulk_desc	*bci_desc;
	crt_bulk_cb_t		bci_cb;
	void			*bci_arg;
};

static hg_return_t
crt_hg_bulk_transfer_cb(const struct hg_cb_info *hg_cbinfo)
{
	struct crt_hg_bulk_cbinfo	*bulk_cbinfo;
	struct crt_bulk_cb_info		crt_bulk_cbinfo;
	struct crt_context		*ctx;
	struct crt_hg_context		*hg_ctx;
	struct crt_bulk_desc		*bulk_desc;
	hg_return_t			hg_ret = HG_SUCCESS;
	int				rc = 0;

	D_ASSERT(hg_cbinfo != NULL);
	bulk_cbinfo = hg_cbinfo->arg;
	D_ASSERT(bulk_cbinfo != NULL);
	bulk_desc = bulk_cbinfo->bci_desc;
	D_ASSERT(bulk_desc != NULL);
	ctx = bulk_desc->bd_rpc->cr_ctx;
	hg_ctx = &ctx->cc_hg_ctx;
	D_ASSERT(hg_ctx != NULL);
	D_ASSERT(hg_cbinfo->type == HG_CB_BULK);
	D_ASSERT(hg_cbinfo->info.bulk.origin_handle ==
		 bulk_desc->bd_remote_hdl);
	D_ASSERT(hg_cbinfo->info.bulk.local_handle ==
		 bulk_desc->bd_local_hdl);

	if (hg_cbinfo->ret != HG_SUCCESS) {
		if (hg_cbinfo->ret == HG_CANCELED) {
			D_DEBUG(DB_NET, "bulk transferring canceled.\n");
			rc = -DER_CANCELED;
		} else {
			D_ERROR("crt_hg_bulk_transfer_cb,hg_cbinfo->ret: %d.\n",
				hg_cbinfo->ret);
			hg_ret = hg_cbinfo->ret;
			rc = -DER_HG;
		}
	}

	if (bulk_cbinfo->bci_cb == NULL) {
		D_DEBUG(DB_NET, "No bulk completion callback registered.\n");
		D_GOTO(out, hg_ret);
	}
	crt_bulk_cbinfo.bci_arg = bulk_cbinfo->bci_arg;
	crt_bulk_cbinfo.bci_rc = rc;
	crt_bulk_cbinfo.bci_bulk_desc = bulk_desc;

	rc = bulk_cbinfo->bci_cb(&crt_bulk_cbinfo);
	if (rc != 0)
		D_ERROR("bulk_cbinfo->bci_cb failed, rc: %d.\n", rc);

out:
	D_FREE_PTR(bulk_cbinfo);
	D_FREE_PTR(bulk_desc);
	return hg_ret;
}

int
crt_hg_bulk_transfer(struct crt_bulk_desc *bulk_desc, crt_bulk_cb_t complete_cb,
		     void *arg, crt_bulk_opid_t *opid, bool bind)
{
	struct crt_context		*ctx;
	struct crt_hg_context		*hg_ctx;
	struct crt_hg_bulk_cbinfo	*bulk_cbinfo;
	hg_bulk_op_t			hg_bulk_op;
	struct crt_bulk_desc		*bulk_desc_dup;
	struct crt_rpc_priv		*rpc_priv;
	hg_return_t			hg_ret = HG_SUCCESS;
	int				rc = 0;

	D_ASSERT(bulk_desc != NULL);
	D_ASSERT(bulk_desc->bd_bulk_op == CRT_BULK_PUT ||
		 bulk_desc->bd_bulk_op == CRT_BULK_GET);
	D_ASSERT(bulk_desc->bd_rpc != NULL);
	ctx = bulk_desc->bd_rpc->cr_ctx;
	hg_ctx = &ctx->cc_hg_ctx;
	D_ASSERT(hg_ctx != NULL && hg_ctx->chc_bulkctx != NULL);

	D_ALLOC_PTR(bulk_cbinfo);
	if (bulk_cbinfo == NULL)
		D_GOTO(out, rc = -DER_NOMEM);
	D_ALLOC_PTR(bulk_desc_dup);
	if (bulk_desc_dup == NULL) {
		D_FREE_PTR(bulk_cbinfo);
		D_GOTO(out, rc = -DER_NOMEM);
	}
	crt_bulk_desc_dup(bulk_desc_dup, bulk_desc);

	bulk_cbinfo->bci_desc = bulk_desc_dup;
	bulk_cbinfo->bci_cb = complete_cb;
	bulk_cbinfo->bci_arg = arg;

	hg_bulk_op = (bulk_desc->bd_bulk_op == CRT_BULK_PUT) ?
		     HG_BULK_PUSH : HG_BULK_PULL;
	rpc_priv = container_of(bulk_desc->bd_rpc, struct crt_rpc_priv,
				crp_pub);
	if (bind)
		hg_ret = HG_Bulk_bind_transfer(hg_ctx->chc_bulkctx,
				crt_hg_bulk_transfer_cb, bulk_cbinfo,
				hg_bulk_op, bulk_desc->bd_remote_hdl,
				bulk_desc->bd_remote_off,
				bulk_desc->bd_local_hdl,
				bulk_desc->bd_local_off,
				bulk_desc->bd_len,
				opid != NULL ? (hg_op_id_t *)opid :
				HG_OP_ID_IGNORE);
	else
		hg_ret = HG_Bulk_transfer_id(hg_ctx->chc_bulkctx,
				crt_hg_bulk_transfer_cb, bulk_cbinfo,
				hg_bulk_op, rpc_priv->crp_hg_addr,
				HG_Get_info(rpc_priv->crp_hg_hdl)->context_id,
				bulk_desc->bd_remote_hdl,
				bulk_desc->bd_remote_off,
				bulk_desc->bd_local_hdl,
				bulk_desc->bd_local_off,
				bulk_desc->bd_len,
				opid != NULL ? (hg_op_id_t *)opid :
				HG_OP_ID_IGNORE);
	if (hg_ret != HG_SUCCESS) {
		D_ERROR("HG_Bulk_(bind)transfer failed, hg_ret: %d.\n", hg_ret);
		D_FREE_PTR(bulk_cbinfo);
		D_FREE_PTR(bulk_desc_dup);
		rc = crt_hgret_2_der(hg_ret);
	}

out:
	return rc;
}
