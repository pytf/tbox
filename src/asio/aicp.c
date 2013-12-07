/*!The Treasure Box Library
 * 
 * TBox is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 * 
 * TBox is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 * 
 * You should have received a copy of the GNU Lesser General Public License
 * along with TBox; 
 * If not, see <a href="http://www.gnu.org/licenses/"> http://www.gnu.org/licenses/</a>
 * 
 * Copyright (C) 2009 - 2012, ruki All rights reserved.
 *
 * @author		ruki
 * @file		aicp.c
 * @ingroup 	asio
 *
 */

/* ///////////////////////////////////////////////////////////////////////
 * includes
 */
#include "aicp.h"
#include "aioo.h"
#include "../math/math.h"
#include "../utils/utils.h"
#include "../memory/memory.h"
#include "../platform/platform.h"

/* ///////////////////////////////////////////////////////////////////////
 * declaration
 */
tb_aicp_proactor_t* tb_aicp_proactor_init(tb_aicp_t* aicp);

/* ///////////////////////////////////////////////////////////////////////
 * aico
 */
static tb_aico_t* tb_aicp_aico_init(tb_aicp_t* aicp, tb_handle_t handle, tb_size_t type)
{
	// check
	tb_assert_and_check_return_val(aicp && handle && type, tb_null);

	// enter 
	if (aicp->lock) tb_spinlock_enter(aicp->lock);

	// make aico
	tb_aico_t* aico = aicp->pool? (tb_aico_t*)tb_rpool_malloc0(aicp->pool) : tb_null;

	// init aico
	if (aico)
	{
		aico->aicp 		= aicp;
		aico->type 		= type;
		aico->handle 	= handle;

		// init timeout 
		tb_size_t i = 0;
		tb_size_t n = tb_arrayn(aico->timeout);
		for (i = 0; i < n; i++) aico->timeout[i] = -1;
	}

	// leave 
	if (aicp->lock) tb_spinlock_leave(aicp->lock);
	
	// ok?
	return aico;
}
static tb_void_t tb_aicp_aico_exit(tb_aicp_t* aicp, tb_aico_t* aico)
{
	// check
	tb_assert_and_check_return(aicp);

	// enter 
	if (aicp->lock) tb_spinlock_enter(aicp->lock);

	// exit it
	if (aico) tb_rpool_free(aicp->pool, aico);

	// leave 
	if (aicp->lock) tb_spinlock_leave(aicp->lock);
}

/* ///////////////////////////////////////////////////////////////////////
 * interfaces
 */
tb_aicp_t* tb_aicp_init(tb_size_t maxn)
{
	// check
	tb_assert_and_check_return_val(maxn, tb_null);

	// alloc aicp
	tb_aicp_t* aicp = tb_malloc0(sizeof(tb_aicp_t));
	tb_assert_and_check_return_val(aicp, tb_null);

	// init aicp
	aicp->maxn = maxn;
	aicp->kill = 0;

	// init proactor
	aicp->ptor = tb_aicp_proactor_init(aicp);
	tb_assert_and_check_goto(aicp->ptor && aicp->ptor->step >= sizeof(tb_aico_t), fail);

	// init lock
	aicp->lock = tb_spinlock_init();
	tb_assert_and_check_goto(aicp->lock, fail);

	// init pool
	aicp->pool = tb_rpool_init((maxn >> 2) + 16, aicp->ptor->step, 0);
	tb_assert_and_check_goto(aicp->pool, fail);

	// ok
	return aicp;

fail:
	if (aicp) tb_aicp_exit(aicp);
	return tb_null;
}
tb_void_t tb_aicp_exit(tb_aicp_t* aicp)
{
	if (aicp)
	{
		// kill all
		if (!tb_atomic_get(&aicp->kill)) tb_aicp_kill(aicp);
	
		// wait workers 
		tb_hong_t time = tb_mclock();
		while (tb_atomic_get(&aicp->work) && (tb_mclock() < time + 5000)) tb_msleep(500);

		// exit proactor
		if (aicp->ptor)
		{
			tb_assert(aicp->ptor && aicp->ptor->exit);
			aicp->ptor->exit(aicp->ptor);
			aicp->ptor = tb_null;
		}

		// exit pool
		if (aicp->lock) tb_spinlock_enter(aicp->lock);
		if (aicp->pool) tb_rpool_exit(aicp->pool);
		aicp->pool = tb_null;
		if (aicp->lock) tb_spinlock_leave(aicp->lock);

		// exit lock
		if (aicp->lock) tb_spinlock_exit(aicp->lock);
		aicp->lock = tb_null;

		// free aicp
		tb_free(aicp);
	}
}
tb_handle_t tb_aicp_addo(tb_aicp_t* aicp, tb_handle_t handle, tb_size_t type)
{
	// check
	tb_assert_and_check_return_val(aicp && aicp->ptor && aicp->ptor->addo && handle && type, tb_null);

	// done
	tb_bool_t 	ok = tb_false;
	tb_aico_t* 	aico = tb_null;
	do
	{
		// init aico
		aico = tb_aicp_aico_init(aicp, handle, type);
		tb_assert_and_check_break(aico);

		// addo aico
		if (!aicp->ptor->addo(aicp->ptor, aico)) break;

		// ok
		ok = tb_true;

	} while (0);

	// failed? remove aico
	if (!ok && aico) 
	{
		tb_aicp_aico_exit(aicp, aico);
		aico = tb_null;
	}

	// ok?
	return (tb_handle_t)aico;
}
tb_void_t tb_aicp_delo(tb_aicp_t* aicp, tb_handle_t aico)
{
	// check
	tb_assert_and_check_return(aicp && aicp->ptor && aicp->ptor->delo && aico);

	// delo
	if (aicp->ptor->delo(aicp->ptor, aico))
		tb_aicp_aico_exit(aicp, aico);
}
tb_bool_t tb_aicp_post(tb_aicp_t* aicp, tb_aice_t const* list, tb_size_t size)
{
	// check
	tb_assert_and_check_return_val(aicp && aicp->ptor && aicp->ptor->post, tb_false);
	tb_assert_and_check_return_val(list && size && size <= TB_AICP_POST_MAXN, tb_false);

	// post++
#ifdef __tb_debug__
	{
		tb_size_t i = 0;
		for (i = 0; i < size; i++)
		{
			// the aico
			tb_aico_t* aico = list[i].aico;
			tb_assert_and_check_return_val(aico, tb_false);

			// post++
			tb_assert_return_val(!tb_atomic_fetch_and_inc(&aico->post), tb_false);
		}
	}
#endif

	// post
	return aicp->ptor->post(aicp->ptor, list, size);
}
tb_void_t tb_aicp_loop(tb_aicp_t* aicp)
{
	// check
	tb_assert_and_check_return(aicp && aicp->ptor);

	// the spak
	tb_long_t (*spak)(tb_aicp_proactor_t* , tb_aice_t* , tb_long_t ) = aicp->ptor->spak;
	tb_assert_and_check_return(spak);

	// worker++
	tb_atomic_fetch_and_inc(&aicp->work);

	// loop
	while (!tb_atomic_get(&aicp->kill))
	{
		// spak
		tb_aice_t 	resp = {0};
		tb_long_t	ok = spak(aicp->ptor, &resp, -1);

		// failed? exit all loops
		if (ok < 0) tb_aicp_kill(aicp);

		// killed? break it
		tb_check_break(!tb_atomic_get(&aicp->kill));
		
		// timeout?
		tb_check_continue(ok);

		// post--
		tb_assert(resp.aico? (tb_atomic_fetch_and_set0(&resp.aico->post) == 1) : 0);

		// done aicb
		if (resp.aicb && !resp.aicb(&resp)) 
		{
			// exit all loops
			tb_aicp_kill(aicp);
			break;
		}
	}

	// worker--
	tb_atomic_fetch_and_dec(&aicp->work);
}
tb_void_t tb_aicp_kill(tb_aicp_t* aicp)
{
	// check
	tb_assert_and_check_return(aicp);

	// kill it
	if (!tb_atomic_fetch_and_set(&aicp->kill, 1))
	{
		// kill proactor
		if (aicp->ptor && aicp->ptor->kill) aicp->ptor->kill(aicp->ptor);
	}
}

