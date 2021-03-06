/*
 * Copyright (c) 2016-2018 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stddef.h>
#include <netinet/in.h>

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/pg/pg.h>
#include <vppinfra/error.h>


#include <acl/acl.h>
#include <vnet/ip/icmp46_packet.h>

#include <plugins/acl/fa_node.h>
#include <plugins/acl/acl.h>
#include <plugins/acl/lookup_context.h>
#include <plugins/acl/public_inlines.h>
#include <plugins/acl/session_inlines.h>

#include <vppinfra/bihash_40_8.h>
#include <vppinfra/bihash_template.h>

typedef struct
{
  u32 next_index;
  u32 sw_if_index;
  u32 lc_index;
  u32 match_acl_in_index;
  u32 match_rule_index;
  u64 packet_info[6];
  u32 trace_bitmap;
  u8 action;
} acl_fa_trace_t;

/* *INDENT-OFF* */
#define foreach_acl_fa_error \
_(ACL_DROP, "ACL deny packets")  \
_(ACL_PERMIT, "ACL permit packets")  \
_(ACL_NEW_SESSION, "new sessions added") \
_(ACL_EXIST_SESSION, "existing session packets") \
_(ACL_CHECK, "checked packets") \
_(ACL_RESTART_SESSION_TIMER, "restart session timer") \
_(ACL_TOO_MANY_SESSIONS, "too many sessions to add new") \
/* end  of errors */

typedef enum
{
#define _(sym,str) ACL_FA_ERROR_##sym,
  foreach_acl_fa_error
#undef _
    ACL_FA_N_ERROR,
} acl_fa_error_t;

/* *INDENT-ON* */


always_inline uword
acl_fa_node_fn (vlib_main_t * vm,
		vlib_node_runtime_t * node, vlib_frame_t * frame, int is_ip6,
		int is_input, int is_l2_path, u32 * l2_feat_next_node_index,
		vlib_node_registration_t * acl_fa_node)
{
  u32 n_left_from, *from, *to_next;
  acl_fa_next_t next_index;
  u32 pkts_acl_checked = 0;
  u32 pkts_new_session = 0;
  u32 pkts_exist_session = 0;
  u32 pkts_acl_permit = 0;
  u32 pkts_restart_session_timer = 0;
  u32 trace_bitmap = 0;
  acl_main_t *am = &acl_main;
  fa_5tuple_t fa_5tuple, kv_sess;
  clib_bihash_kv_40_8_t value_sess;
  vlib_node_runtime_t *error_node;
  u64 now = clib_cpu_time_now ();
  uword thread_index = os_get_thread_index ();
  acl_fa_per_worker_data_t *pw = &am->per_worker_data[thread_index];

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;

  error_node = vlib_node_get_runtime (vm, acl_fa_node->index);

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  u32 bi0;
	  vlib_buffer_t *b0;
	  u32 next0 = 0;
	  u8 action = 0;
	  u32 sw_if_index0;
	  u32 lc_index0;
	  int acl_check_needed = 1;
	  u32 match_acl_in_index = ~0;
	  u32 match_acl_pos = ~0;
	  u32 match_rule_index = ~0;
	  u8 error0 = 0;
	  u32 valid_new_sess;

	  /* speculatively enqueue b0 to the current next frame */
	  bi0 = from[0];
	  to_next[0] = bi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  b0 = vlib_get_buffer (vm, bi0);

	  if (is_input)
	    sw_if_index0 = vnet_buffer (b0)->sw_if_index[VLIB_RX];
	  else
	    sw_if_index0 = vnet_buffer (b0)->sw_if_index[VLIB_TX];

	  if (is_input)
	    lc_index0 = am->input_lc_index_by_sw_if_index[sw_if_index0];
	  else
	    lc_index0 = am->output_lc_index_by_sw_if_index[sw_if_index0];

	  u32 **p_epoch_vec =
	    is_input ? &am->input_policy_epoch_by_sw_if_index :
	    &am->output_policy_epoch_by_sw_if_index;
	  u16 current_policy_epoch =
	    sw_if_index0 < vec_len (*p_epoch_vec) ? vec_elt (*p_epoch_vec,
							     sw_if_index0)
	    : (is_input * FA_POLICY_EPOCH_IS_INPUT);
	  /*
	   * Extract the L3/L4 matching info into a 5-tuple structure,
	   * then create a session key whose layout is independent on forward or reverse
	   * direction of the packet.
	   */

	  acl_plugin_fill_5tuple_inline (lc_index0, b0, is_ip6, is_input,
					 is_l2_path,
					 (fa_5tuple_opaque_t *) & fa_5tuple);
	  fa_5tuple.l4.lsb_of_sw_if_index = sw_if_index0 & 0xffff;
	  valid_new_sess =
	    acl_make_5tuple_session_key (am, is_input, is_ip6, sw_if_index0,
					 &fa_5tuple, &kv_sess);
	  // XXDEL fa_5tuple.pkt.is_input = is_input;
	  fa_5tuple.pkt.mask_type_index_lsb = ~0;
#ifdef FA_NODE_VERBOSE_DEBUG
	  clib_warning
	    ("ACL_FA_NODE_DBG: session 5-tuple %016llx %016llx %016llx %016llx %016llx %016llx",
	     kv_sess.kv.key[0], kv_sess.kv.key[1], kv_sess.kv.key[2],
	     kv_sess.kv.key[3], kv_sess.kv.key[4], kv_sess.kv.value);
	  clib_warning
	    ("ACL_FA_NODE_DBG: packet 5-tuple %016llx %016llx %016llx %016llx %016llx %016llx",
	     fa_5tuple.kv.key[0], fa_5tuple.kv.key[1], fa_5tuple.kv.key[2],
	     fa_5tuple.kv.key[3], fa_5tuple.kv.key[4], fa_5tuple.kv.value);
#endif

	  /* Try to match an existing session first */

	  if (acl_fa_ifc_has_sessions (am, sw_if_index0))
	    {
	      if (acl_fa_find_session
		  (am, sw_if_index0, &kv_sess, &value_sess))
		{
		  trace_bitmap |= 0x80000000;
		  error0 = ACL_FA_ERROR_ACL_EXIST_SESSION;
		  fa_full_session_id_t f_sess_id;

		  f_sess_id.as_u64 = value_sess.value;
		  ASSERT (f_sess_id.thread_index < vec_len (vlib_mains));

		  fa_session_t *sess =
		    get_session_ptr (am, f_sess_id.thread_index,
				     f_sess_id.session_index);
		  int old_timeout_type =
		    fa_session_get_timeout_type (am, sess);
		  action =
		    acl_fa_track_session (am, is_input, sw_if_index0, now,
					  sess, &fa_5tuple);
		  /* expose the session id to the tracer */
		  match_rule_index = f_sess_id.session_index;
		  int new_timeout_type =
		    fa_session_get_timeout_type (am, sess);
		  acl_check_needed = 0;
		  pkts_exist_session += 1;
		  /* Tracking might have changed the session timeout type, e.g. from transient to established */
		  if (PREDICT_FALSE (old_timeout_type != new_timeout_type))
		    {
		      acl_fa_restart_timer_for_session (am, now, f_sess_id);
		      pkts_restart_session_timer++;
		      trace_bitmap |=
			0x00010000 + ((0xff & old_timeout_type) << 8) +
			(0xff & new_timeout_type);
		    }
		  /*
		   * I estimate the likelihood to be very low - the VPP needs
		   * to have >64K interfaces to start with and then on
		   * exactly 64K indices apart needs to be exactly the same
		   * 5-tuple... Anyway, since this probability is nonzero -
		   * print an error and drop the unlucky packet.
		   * If this shows up in real world, we would need to bump
		   * the hash key length.
		   */
		  if (PREDICT_FALSE (sess->sw_if_index != sw_if_index0))
		    {
		      clib_warning
			("BUG: session LSB16(sw_if_index) and 5-tuple collision!");
		      acl_check_needed = 0;
		      action = 0;
		    }
		  if (PREDICT_FALSE (am->reclassify_sessions))
		    {
		      /* if the MSB of policy epoch matches but not the LSB means it is a stale session */
		      if ((0 ==
			   ((current_policy_epoch ^
			     f_sess_id.intf_policy_epoch) &
			    FA_POLICY_EPOCH_IS_INPUT))
			  && (current_policy_epoch !=
			      f_sess_id.intf_policy_epoch))
			{
			  /* delete session and increment the counter */
			  vec_validate
			    (pw->fa_session_epoch_change_by_sw_if_index,
			     sw_if_index0);
			  vec_elt (pw->fa_session_epoch_change_by_sw_if_index,
				   sw_if_index0)++;
			  if (acl_fa_conn_list_delete_session (am, f_sess_id))
			    {
			      /* delete the session only if we were able to unlink it */
			      acl_fa_delete_session (am, sw_if_index0,
						     f_sess_id);
			    }
			  acl_check_needed = 1;
			  trace_bitmap |= 0x40000000;
			}
		    }
		}
	    }

	  if (acl_check_needed)
	    {
	      action = 0;	/* deny by default */
	      acl_plugin_match_5tuple_inline (lc_index0,
					      (fa_5tuple_opaque_t *) &
					      fa_5tuple, is_ip6, &action,
					      &match_acl_pos,
					      &match_acl_in_index,
					      &match_rule_index,
					      &trace_bitmap);
	      error0 = action;
	      if (1 == action)
		pkts_acl_permit += 1;
	      if (2 == action)
		{
		  if (!acl_fa_can_add_session (am, is_input, sw_if_index0))
		    acl_fa_try_recycle_session (am, is_input, thread_index,
						sw_if_index0);

		  if (acl_fa_can_add_session (am, is_input, sw_if_index0))
		    {
		      if (PREDICT_TRUE (valid_new_sess))
			{
			  fa_session_t *sess =
			    acl_fa_add_session (am, is_input,
						sw_if_index0,
						now, &kv_sess,
						current_policy_epoch);
			  acl_fa_track_session (am, is_input, sw_if_index0,
						now, sess, &fa_5tuple);
			  pkts_new_session += 1;
			}
		      else
			{
			  /*
			   *  ICMP packets with non-icmp_valid_new type will be
			   *  forwared without being dropped.
			   */
			  action = 1;
			  pkts_acl_permit += 1;
			}
		    }
		  else
		    {
		      action = 0;
		      error0 = ACL_FA_ERROR_ACL_TOO_MANY_SESSIONS;
		    }
		}
	    }



	  if (action > 0)
	    {
	      if (is_l2_path)
		next0 = vnet_l2_feature_next (b0, l2_feat_next_node_index, 0);
	      else
		vnet_feature_next (sw_if_index0, &next0, b0);
	    }
#ifdef FA_NODE_VERBOSE_DEBUG
	  clib_warning
	    ("ACL_FA_NODE_DBG: sw_if_index %d lc_index %d action %d acl_index %d rule_index %d",
	     sw_if_index0, lc_index0, action, match_acl_in_index,
	     match_rule_index);
#endif

	  if (PREDICT_FALSE ((node->flags & VLIB_NODE_FLAG_TRACE)
			     && (b0->flags & VLIB_BUFFER_IS_TRACED)))
	    {
	      acl_fa_trace_t *t = vlib_add_trace (vm, node, b0, sizeof (*t));
	      t->sw_if_index = sw_if_index0;
	      t->lc_index = lc_index0;
	      t->next_index = next0;
	      t->match_acl_in_index = match_acl_in_index;
	      t->match_rule_index = match_rule_index;
	      t->packet_info[0] = fa_5tuple.kv.key[0];
	      t->packet_info[1] = fa_5tuple.kv.key[1];
	      t->packet_info[2] = fa_5tuple.kv.key[2];
	      t->packet_info[3] = fa_5tuple.kv.key[3];
	      t->packet_info[4] = fa_5tuple.kv.key[4];
	      t->packet_info[5] = fa_5tuple.kv.value;
	      t->action = action;
	      t->trace_bitmap = trace_bitmap;
	    }

	  next0 = next0 < node->n_next_nodes ? next0 : 0;
	  if (0 == next0)
	    b0->error = error_node->errors[error0];

	  pkts_acl_checked += 1;

	  /* verify speculative enqueue, maybe switch current next frame */
	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next, bi0,
					   next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  vlib_node_increment_counter (vm, acl_fa_node->index,
			       ACL_FA_ERROR_ACL_CHECK, pkts_acl_checked);
  vlib_node_increment_counter (vm, acl_fa_node->index,
			       ACL_FA_ERROR_ACL_PERMIT, pkts_acl_permit);
  vlib_node_increment_counter (vm, acl_fa_node->index,
			       ACL_FA_ERROR_ACL_NEW_SESSION,
			       pkts_new_session);
  vlib_node_increment_counter (vm, acl_fa_node->index,
			       ACL_FA_ERROR_ACL_EXIST_SESSION,
			       pkts_exist_session);
  vlib_node_increment_counter (vm, acl_fa_node->index,
			       ACL_FA_ERROR_ACL_RESTART_SESSION_TIMER,
			       pkts_restart_session_timer);
  return frame->n_vectors;
}

vlib_node_function_t __clib_weak acl_in_ip4_l2_node_fn_avx512;
vlib_node_function_t __clib_weak acl_in_ip4_l2_node_fn_avx2;

vlib_node_function_t __clib_weak acl_out_ip4_l2_node_fn_avx512;
vlib_node_function_t __clib_weak acl_out_ip4_l2_node_fn_avx2;

vlib_node_function_t __clib_weak acl_in_ip6_l2_node_fn_avx512;
vlib_node_function_t __clib_weak acl_in_ip6_l2_node_fn_avx2;

vlib_node_function_t __clib_weak acl_out_ip6_l2_node_fn_avx512;
vlib_node_function_t __clib_weak acl_out_ip6_l2_node_fn_avx2;

vlib_node_function_t __clib_weak acl_in_ip4_fa_node_fn_avx512;
vlib_node_function_t __clib_weak acl_in_ip4_fa_node_fn_avx2;

vlib_node_function_t __clib_weak acl_out_ip4_fa_node_fn_avx512;
vlib_node_function_t __clib_weak acl_out_ip4_fa_node_fn_avx2;

vlib_node_function_t __clib_weak acl_in_ip6_fa_node_fn_avx512;
vlib_node_function_t __clib_weak acl_in_ip6_fa_node_fn_avx2;

vlib_node_function_t __clib_weak acl_out_ip6_fa_node_fn_avx512;
vlib_node_function_t __clib_weak acl_out_ip6_fa_node_fn_avx2;


vlib_node_registration_t acl_in_l2_ip6_node;
uword CLIB_CPU_OPTIMIZED
CLIB_MULTIARCH_FN (acl_in_ip6_l2_node_fn) (vlib_main_t * vm,
					   vlib_node_runtime_t * node,
					   vlib_frame_t * frame)
{
  acl_main_t *am = &acl_main;
  return acl_fa_node_fn (vm, node, frame, 1, 1, 1,
			 am->fa_acl_in_ip6_l2_node_feat_next_node_index,
			 &acl_in_l2_ip6_node);
}

vlib_node_registration_t acl_in_l2_ip4_node;
uword CLIB_CPU_OPTIMIZED
CLIB_MULTIARCH_FN (acl_in_ip4_l2_node_fn) (vlib_main_t * vm,
					   vlib_node_runtime_t * node,
					   vlib_frame_t * frame)
{
  acl_main_t *am = &acl_main;
  return acl_fa_node_fn (vm, node, frame, 0, 1, 1,
			 am->fa_acl_in_ip4_l2_node_feat_next_node_index,
			 &acl_in_l2_ip4_node);
}

vlib_node_registration_t acl_out_l2_ip6_node;
uword CLIB_CPU_OPTIMIZED
CLIB_MULTIARCH_FN (acl_out_ip6_l2_node_fn) (vlib_main_t * vm,
					    vlib_node_runtime_t * node,
					    vlib_frame_t * frame)
{
  acl_main_t *am = &acl_main;
  return acl_fa_node_fn (vm, node, frame, 1, 0, 1,
			 am->fa_acl_out_ip6_l2_node_feat_next_node_index,
			 &acl_out_l2_ip6_node);
}

vlib_node_registration_t acl_out_l2_ip4_node;
uword CLIB_CPU_OPTIMIZED
CLIB_MULTIARCH_FN (acl_out_ip4_l2_node_fn) (vlib_main_t * vm,
					    vlib_node_runtime_t * node,
					    vlib_frame_t * frame)
{
  acl_main_t *am = &acl_main;
  return acl_fa_node_fn (vm, node, frame, 0, 0, 1,
			 am->fa_acl_out_ip4_l2_node_feat_next_node_index,
			 &acl_out_l2_ip4_node);
}

/**** L3 processing path nodes ****/

vlib_node_registration_t acl_in_fa_ip6_node;
uword CLIB_CPU_OPTIMIZED
CLIB_MULTIARCH_FN (acl_in_ip6_fa_node_fn) (vlib_main_t * vm,
					   vlib_node_runtime_t * node,
					   vlib_frame_t * frame)
{
  return acl_fa_node_fn (vm, node, frame, 1, 1, 0, 0, &acl_in_fa_ip6_node);
}

vlib_node_registration_t acl_in_fa_ip4_node;
uword CLIB_CPU_OPTIMIZED
CLIB_MULTIARCH_FN (acl_in_ip4_fa_node_fn) (vlib_main_t * vm,
					   vlib_node_runtime_t * node,
					   vlib_frame_t * frame)
{
  return acl_fa_node_fn (vm, node, frame, 0, 1, 0, 0, &acl_in_fa_ip4_node);
}

vlib_node_registration_t acl_out_fa_ip6_node;
uword CLIB_CPU_OPTIMIZED
CLIB_MULTIARCH_FN (acl_out_ip6_fa_node_fn) (vlib_main_t * vm,
					    vlib_node_runtime_t * node,
					    vlib_frame_t * frame)
{
  return acl_fa_node_fn (vm, node, frame, 1, 0, 0, 0, &acl_out_fa_ip6_node);
}

vlib_node_registration_t acl_out_fa_ip4_node;
uword CLIB_CPU_OPTIMIZED
CLIB_MULTIARCH_FN (acl_out_ip4_fa_node_fn) (vlib_main_t * vm,
					    vlib_node_runtime_t * node,
					    vlib_frame_t * frame)
{
  return acl_fa_node_fn (vm, node, frame, 0, 0, 0, 0, &acl_out_fa_ip4_node);
}



#if __x86_64__
static void __clib_constructor
acl_plugin_multiarch_select (void)
{
  if (acl_in_ip4_l2_node_fn_avx512 && clib_cpu_supports_avx512f ())
    acl_in_l2_ip4_node.function = acl_in_ip4_l2_node_fn_avx512;
  else if (acl_in_ip4_l2_node_fn_avx2 && clib_cpu_supports_avx2 ())
    acl_in_l2_ip4_node.function = acl_in_ip4_l2_node_fn_avx2;

  if (acl_out_ip4_l2_node_fn_avx512 && clib_cpu_supports_avx512f ())
    acl_out_l2_ip4_node.function = acl_out_ip4_l2_node_fn_avx512;
  else if (acl_out_ip4_l2_node_fn_avx2 && clib_cpu_supports_avx2 ())
    acl_out_l2_ip4_node.function = acl_out_ip4_l2_node_fn_avx2;

  if (acl_in_ip6_l2_node_fn_avx512 && clib_cpu_supports_avx512f ())
    acl_in_l2_ip6_node.function = acl_in_ip6_l2_node_fn_avx512;
  else if (acl_in_ip6_l2_node_fn_avx2 && clib_cpu_supports_avx2 ())
    acl_in_l2_ip6_node.function = acl_in_ip6_l2_node_fn_avx2;

  if (acl_out_ip6_l2_node_fn_avx512 && clib_cpu_supports_avx512f ())
    acl_out_l2_ip6_node.function = acl_out_ip6_l2_node_fn_avx512;
  else if (acl_out_ip6_l2_node_fn_avx2 && clib_cpu_supports_avx2 ())
    acl_out_l2_ip6_node.function = acl_out_ip6_l2_node_fn_avx2;

  if (acl_in_ip4_fa_node_fn_avx512 && clib_cpu_supports_avx512f ())
    acl_in_fa_ip4_node.function = acl_in_ip4_fa_node_fn_avx512;
  else if (acl_in_ip4_fa_node_fn_avx2 && clib_cpu_supports_avx2 ())
    acl_in_fa_ip4_node.function = acl_in_ip4_fa_node_fn_avx2;

  if (acl_out_ip4_fa_node_fn_avx512 && clib_cpu_supports_avx512f ())
    acl_out_fa_ip4_node.function = acl_out_ip4_fa_node_fn_avx512;
  else if (acl_out_ip4_fa_node_fn_avx2 && clib_cpu_supports_avx2 ())
    acl_out_fa_ip4_node.function = acl_out_ip4_fa_node_fn_avx2;

  if (acl_in_ip6_fa_node_fn_avx512 && clib_cpu_supports_avx512f ())
    acl_in_fa_ip6_node.function = acl_in_ip6_fa_node_fn_avx512;
  else if (acl_in_ip6_fa_node_fn_avx2 && clib_cpu_supports_avx2 ())
    acl_in_fa_ip6_node.function = acl_in_ip6_fa_node_fn_avx2;

  if (acl_out_ip6_fa_node_fn_avx512 && clib_cpu_supports_avx512f ())
    acl_out_fa_ip6_node.function = acl_out_ip6_fa_node_fn_avx512;
  else if (acl_out_ip6_fa_node_fn_avx2 && clib_cpu_supports_avx2 ())
    acl_out_fa_ip6_node.function = acl_out_ip6_fa_node_fn_avx2;

}
#endif



#ifndef CLIB_MULTIARCH_VARIANT
static u8 *
format_fa_5tuple (u8 * s, va_list * args)
{
  fa_5tuple_t *p5t = va_arg (*args, fa_5tuple_t *);

  return format (s, "lc_index %d (lsb16 of sw_if_index %d) l3 %s%s %U -> %U"
		 " l4 proto %d l4_valid %d port %d -> %d tcp flags (%s) %02x rsvd %x",
		 p5t->pkt.lc_index, p5t->l4.lsb_of_sw_if_index,
		 p5t->pkt.is_ip6 ? "ip6" : "ip4",
		 p5t->pkt.is_nonfirst_fragment ? " non-initial fragment" : "",
		 format_ip46_address, &p5t->addr[0],
		 p5t->pkt.is_ip6 ? IP46_TYPE_IP6 : IP46_TYPE_IP4,
		 format_ip46_address, &p5t->addr[1],
		 p5t->pkt.is_ip6 ? IP46_TYPE_IP6 : IP46_TYPE_IP4,
		 p5t->l4.proto, p5t->pkt.l4_valid, p5t->l4.port[0],
		 p5t->l4.port[1],
		 p5t->pkt.tcp_flags_valid ? "valid" : "invalid",
		 p5t->pkt.tcp_flags, p5t->pkt.flags_reserved);
}

u8 *
format_acl_plugin_5tuple (u8 * s, va_list * args)
{
  return format_fa_5tuple (s, args);
}

/* packet trace format function */
u8 *
format_acl_plugin_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  acl_fa_trace_t *t = va_arg (*args, acl_fa_trace_t *);

  s =
    format (s,
	    "acl-plugin: lc_index: %d, sw_if_index %d, next index %d, action: %d, match: acl %d rule %d trace_bits %08x\n"
	    "  pkt info %016llx %016llx %016llx %016llx %016llx %016llx",
	    t->lc_index, t->sw_if_index, t->next_index, t->action,
	    t->match_acl_in_index, t->match_rule_index, t->trace_bitmap,
	    t->packet_info[0], t->packet_info[1], t->packet_info[2],
	    t->packet_info[3], t->packet_info[4], t->packet_info[5]);

  /* Now also print out the packet_info in a form usable by humans */
  s = format (s, "\n   %U", format_fa_5tuple, t->packet_info);
  return s;
}


/* *INDENT-OFF* */

static char *acl_fa_error_strings[] = {
#define _(sym,string) string,
  foreach_acl_fa_error
#undef _
};

VLIB_REGISTER_NODE (acl_in_l2_ip6_node) =
{
  .function = acl_in_ip6_l2_node_fn,
  .name = "acl-plugin-in-ip6-l2",
  .vector_size = sizeof (u32),
  .format_trace = format_acl_plugin_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (acl_fa_error_strings),
  .error_strings = acl_fa_error_strings,
  .n_next_nodes = ACL_FA_N_NEXT,
  .next_nodes =
  {
    [ACL_FA_ERROR_DROP] = "error-drop",
  }
};

VLIB_REGISTER_NODE (acl_in_l2_ip4_node) =
{
  .function = acl_in_ip4_l2_node_fn,
  .name = "acl-plugin-in-ip4-l2",
  .vector_size = sizeof (u32),
  .format_trace = format_acl_plugin_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (acl_fa_error_strings),
  .error_strings = acl_fa_error_strings,
  .n_next_nodes = ACL_FA_N_NEXT,
  .next_nodes =
  {
    [ACL_FA_ERROR_DROP] = "error-drop",
  }
};

VLIB_REGISTER_NODE (acl_out_l2_ip6_node) =
{
  .function = acl_out_ip6_l2_node_fn,
  .name = "acl-plugin-out-ip6-l2",
  .vector_size = sizeof (u32),
  .format_trace = format_acl_plugin_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (acl_fa_error_strings),
  .error_strings = acl_fa_error_strings,
  .n_next_nodes = ACL_FA_N_NEXT,
  .next_nodes =
  {
    [ACL_FA_ERROR_DROP] = "error-drop",
  }
};

VLIB_REGISTER_NODE (acl_out_l2_ip4_node) =
{
  .function = acl_out_ip4_l2_node_fn,
  .name = "acl-plugin-out-ip4-l2",
  .vector_size = sizeof (u32),
  .format_trace = format_acl_plugin_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (acl_fa_error_strings),
  .error_strings = acl_fa_error_strings,
  .n_next_nodes = ACL_FA_N_NEXT,
  .next_nodes =
  {
    [ACL_FA_ERROR_DROP] = "error-drop",
  }
};


VLIB_REGISTER_NODE (acl_in_fa_ip6_node) =
{
  .function = acl_in_ip6_fa_node_fn,
  .name = "acl-plugin-in-ip6-fa",
  .vector_size = sizeof (u32),
  .format_trace = format_acl_plugin_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (acl_fa_error_strings),
  .error_strings = acl_fa_error_strings,
  .n_next_nodes = ACL_FA_N_NEXT,
  .next_nodes =
  {
    [ACL_FA_ERROR_DROP] = "error-drop",
  }
};

VNET_FEATURE_INIT (acl_in_ip6_fa_feature, static) =
{
  .arc_name = "ip6-unicast",
  .node_name = "acl-plugin-in-ip6-fa",
  .runs_before = VNET_FEATURES ("ip6-flow-classify"),
};

VLIB_REGISTER_NODE (acl_in_fa_ip4_node) =
{
  .function = acl_in_ip4_fa_node_fn,
  .name = "acl-plugin-in-ip4-fa",
  .vector_size = sizeof (u32),
  .format_trace = format_acl_plugin_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (acl_fa_error_strings),
  .error_strings = acl_fa_error_strings,
  .n_next_nodes = ACL_FA_N_NEXT,
  .next_nodes =
  {
    [ACL_FA_ERROR_DROP] = "error-drop",
  }
};

VNET_FEATURE_INIT (acl_in_ip4_fa_feature, static) =
{
  .arc_name = "ip4-unicast",
  .node_name = "acl-plugin-in-ip4-fa",
  .runs_before = VNET_FEATURES ("ip4-flow-classify"),
};


VLIB_REGISTER_NODE (acl_out_fa_ip6_node) =
{
  .function = acl_out_ip6_fa_node_fn,
  .name = "acl-plugin-out-ip6-fa",
  .vector_size = sizeof (u32),
  .format_trace = format_acl_plugin_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (acl_fa_error_strings),
  .error_strings = acl_fa_error_strings,
  .n_next_nodes = ACL_FA_N_NEXT,
  .next_nodes =
  {
    [ACL_FA_ERROR_DROP] = "error-drop",
  }
};

VNET_FEATURE_INIT (acl_out_ip6_fa_feature, static) =
{
  .arc_name = "ip6-output",
  .node_name = "acl-plugin-out-ip6-fa",
  .runs_before = VNET_FEATURES ("interface-output"),
};

VLIB_REGISTER_NODE (acl_out_fa_ip4_node) =
{
  .function = acl_out_ip4_fa_node_fn,
  .name = "acl-plugin-out-ip4-fa",
  .vector_size = sizeof (u32),
  .format_trace = format_acl_plugin_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = ARRAY_LEN (acl_fa_error_strings),
  .error_strings = acl_fa_error_strings,
  .n_next_nodes = ACL_FA_N_NEXT,
    /* edit / add dispositions here */
  .next_nodes =
  {
    [ACL_FA_ERROR_DROP] = "error-drop",
  }
};

VNET_FEATURE_INIT (acl_out_ip4_fa_feature, static) =
{
  .arc_name = "ip4-output",
  .node_name = "acl-plugin-out-ip4-fa",
  .runs_before = VNET_FEATURES ("interface-output"),
};
#endif

/* *INDENT-ON* */

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
