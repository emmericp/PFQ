/***************************************************************
 *
 * (C) 2011-14 Nicola Bonelli <nicola@pfq.io>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 ****************************************************************/

#ifndef PF_Q_FUNCTIONAL_CONDITIONAL_H
#define PF_Q_FUNCTIONAL_CONDITIONAL_H

#include <pf_q-module.h>

#include "predicate.h"

static inline ActionSkBuff
conditional(arguments_t args, SkBuff b)
{
        predicate_t pred_ = GET_ARG_0(predicate_t, args);
        function_t  then_ = GET_ARG_1(function_t, args);
        function_t  else_ = GET_ARG_2(function_t, args);

	if (EVAL_PREDICATE(pred_, b))
		return EVAL_FUNCTION(then_, b);

	return EVAL_FUNCTION(else_, b);
}

static inline ActionSkBuff
when(arguments_t args, SkBuff b)
{
        predicate_t pred_ = GET_ARG_0(predicate_t, args);
        function_t  fun_  = GET_ARG_1(function_t, args);

	if (EVAL_PREDICATE(pred_, b))
		return EVAL_FUNCTION(fun_, b);

	return Pass(b);
}

static inline ActionSkBuff
unless(arguments_t args, SkBuff b)
{
        predicate_t pred_ = GET_ARG_0(predicate_t, args);
        function_t  fun_  = GET_ARG_1(function_t, args);

	if (EVAL_PREDICATE(pred_, b))
		return Pass(b);

	return EVAL_FUNCTION(fun_, b);
}


#endif /* PF_Q_FUNCTIONAL_CONDITIONAL_H */
