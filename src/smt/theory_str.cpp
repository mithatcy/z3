/*++
Module Name:

    theory_str.cpp

Abstract:

    String Theory Plugin

Author:

    Murphy Berzish (mtrberzi) 2015-09-03

Revision History:

--*/
#include"ast_smt2_pp.h"
#include"smt_context.h"
#include"theory_str.h"
#include"smt_model_generator.h"
#include"ast_pp.h"
#include"ast_ll_pp.h"
#include<list>
#include<vector>
#include<algorithm>
#include"theory_arith.h"

namespace smt {

theory_str::theory_str(ast_manager & m):
        theory(m.mk_family_id("str")),
        /* Options */
        opt_AggressiveLengthTesting(false),
        opt_AggressiveValueTesting(false),
        opt_AggressiveUnrollTesting(true),
        opt_EagerStringConstantLengthAssertions(true),
        opt_VerifyFinalCheckProgress(true),
		opt_LCMUnrollStep(2),
		opt_NoQuickReturn_IntegerTheory(false),
		opt_DisableIntegerTheoryIntegration(false),
		opt_DeferEQCConsistencyCheck(false),
		opt_CheckVariableScope(true),
		opt_UseFastLengthTesterCache(true),
		opt_UseFastValueTesterCache(true),
        /* Internal setup */
        search_started(false),
        m_autil(m),
        m_strutil(m),
        sLevel(0),
        finalCheckProgressIndicator(false),
        m_trail(m),
        m_delayed_axiom_setup_terms(m),
        tmpStringVarCount(0),
		tmpXorVarCount(0),
		tmpLenTestVarCount(0),
		tmpValTestVarCount(0),
		avoidLoopCut(true),
		loopDetected(false),
		contains_map(m),
		m_find(*this),
		m_trail_stack(*this)
{
		initialize_charset();
}

theory_str::~theory_str() {
    m_trail_stack.reset();
}

void theory_str::initialize_charset() {
	bool defaultCharset = true;
	if (defaultCharset) {
		// valid C strings can't contain the null byte ('\0')
		charSetSize = 255;
		char_set = alloc_svect(char, charSetSize);
		int idx = 0;
		// small letters
		for (int i = 97; i < 123; i++) {
			char_set[idx] = (char) i;
			charSetLookupTable[char_set[idx]] = idx;
			idx++;
		}
		// caps
		for (int i = 65; i < 91; i++) {
			char_set[idx] = (char) i;
			charSetLookupTable[char_set[idx]] = idx;
			idx++;
		}
		// numbers
		for (int i = 48; i < 58; i++) {
			char_set[idx] = (char) i;
			charSetLookupTable[char_set[idx]] = idx;
			idx++;
		}
		// printable marks - 1
		for (int i = 32; i < 48; i++) {
			char_set[idx] = (char) i;
			charSetLookupTable[char_set[idx]] = idx;
			idx++;
		}
		// printable marks - 2
		for (int i = 58; i < 65; i++) {
			char_set[idx] = (char) i;
			charSetLookupTable[char_set[idx]] = idx;
			idx++;
		}
		// printable marks - 3
		for (int i = 91; i < 97; i++) {
			char_set[idx] = (char) i;
			charSetLookupTable[char_set[idx]] = idx;
			idx++;
		}
		// printable marks - 4
		for (int i = 123; i < 127; i++) {
			char_set[idx] = (char) i;
			charSetLookupTable[char_set[idx]] = idx;
			idx++;
		}
		// non-printable - 1
		for (int i = 1; i < 32; i++) {
			char_set[idx] = (char) i;
			charSetLookupTable[char_set[idx]] = idx;
			idx++;
		}
		// non-printable - 2
		for (int i = 127; i < 256; i++) {
			char_set[idx] = (char) i;
			charSetLookupTable[char_set[idx]] = idx;
			idx++;
		}
	} else {
		const char setset[] = { 'a', 'b', 'c' };
		int fSize = sizeof(setset) / sizeof(char);

		char_set = alloc_svect(char, fSize);
		charSetSize = fSize;
		for (int i = 0; i < charSetSize; i++) {
			char_set[i] = setset[i];
			charSetLookupTable[setset[i]] = i;
		}
	}
}

void theory_str::assert_axiom(expr * e) {
    if (opt_VerifyFinalCheckProgress) {
        finalCheckProgressIndicator = true;
    }

    if (get_manager().is_true(e)) return;
    TRACE("t_str_detail", tout << "asserting " << mk_ismt2_pp(e, get_manager()) << std::endl;);
    context & ctx = get_context();
    if (!ctx.b_internalized(e)) {
        ctx.internalize(e, false);
    }
    literal lit(ctx.get_literal(e));
    ctx.mark_as_relevant(lit);
    ctx.mk_th_axiom(get_id(), 1, &lit);

    // crash/error avoidance: add all axioms to the trail
    m_trail.push_back(e);

    //TRACE("t_str_detail", tout << "done asserting " << mk_ismt2_pp(e, get_manager()) << std::endl;);
}

expr * theory_str::rewrite_implication(expr * premise, expr * conclusion) {
    ast_manager & m = get_manager();
    return m.mk_or(m.mk_not(premise), conclusion);
}

void theory_str::assert_implication(expr * premise, expr * conclusion) {
    ast_manager & m = get_manager();
    TRACE("t_str_detail", tout << "asserting implication " << mk_ismt2_pp(premise, m) << " -> " << mk_ismt2_pp(conclusion, m) << std::endl;);
    expr_ref axiom(m.mk_or(m.mk_not(premise), conclusion), m);
    assert_axiom(axiom);
}

bool theory_str::internalize_atom(app * atom, bool gate_ctx) {
    /*
    TRACE("t_str", tout << "internalizing atom: " << mk_ismt2_pp(atom, get_manager()) << std::endl;);
    SASSERT(atom->get_family_id() == get_family_id());

    context & ctx = get_context();

    if (ctx.b_internalized(atom))
        return true;

    unsigned num_args = atom->get_num_args();
    for (unsigned i = 0; i < num_args; i++)
        ctx.internalize(atom->get_arg(i), false);

    literal l(ctx.mk_bool_var(atom));

    ctx.set_var_theory(l.var(), get_id());

    return true;
    */
    return internalize_term(atom);
}

bool theory_str::internalize_term(app * term) {
    context & ctx = get_context();
    ast_manager & m = get_manager();
    SASSERT(term->get_family_id() == get_family_id());

    TRACE("t_str_detail", tout << "internalizing term: " << mk_ismt2_pp(term, get_manager()) << std::endl;);

    // emulation of user_smt_theory::internalize_term()

    unsigned num_args = term->get_num_args();
    for (unsigned i = 0; i < num_args; ++i) {
        ctx.internalize(term->get_arg(i), false);
    }
    if (ctx.e_internalized(term)) {
        enode * e = ctx.get_enode(term);
        mk_var(e);
        return true;
    }
    // m_parents.push_back(term);
    enode * e = ctx.mk_enode(term, false, m.is_bool(term), true);
    if (m.is_bool(term)) {
        bool_var bv = ctx.mk_bool_var(term);
        ctx.set_var_theory(bv, get_id());
        ctx.set_enode_flag(bv, true);
    }
    // make sure every argument is attached to a theory variable
    for (unsigned i = 0; i < num_args; ++i) {
        enode * arg = e->get_arg(i);
        theory_var v_arg = mk_var(arg);
        TRACE("t_str_detail", tout << "arg has theory var #" << v_arg << std::endl;);
    }

    theory_var v = mk_var(e);
    TRACE("t_str_detail", tout << "term has theory var #" << v << std::endl;);

    if (opt_EagerStringConstantLengthAssertions && m_strutil.is_string(term)) {
        TRACE("t_str", tout << "eagerly asserting length of string term " << mk_pp(term, m) << std::endl;);
        m_basicstr_axiom_todo.insert(e);
        TRACE("t_str_axiom_bug", tout << "add " << mk_pp(e->get_owner(), m) << " to m_basicstr_axiom_todo" << std::endl;);
    }
    return true;

    /* // what I had before
    SASSERT(!ctx.e_internalized(term));

    unsigned num_args = term->get_num_args();
    for (unsigned i = 0; i < num_args; i++)
        ctx.internalize(term->get_arg(i), false);

    enode * e = (ctx.e_internalized(term)) ? ctx.get_enode(term) :
                                             ctx.mk_enode(term, false, false, true);

    if (is_attached_to_var(e))
        return false;

    attach_new_th_var(e);

    //if (is_concat(term)) {
    //    instantiate_concat_axiom(e);
    //}
    */

    // TODO do we still need to do instantiate_concat_axiom()?

    // partially from theory_seq::internalize_term()
    /*
    if (ctx.e_internalized(term)) {
        enode* e = ctx.get_enode(term);
        mk_var(e);
        return true;
    }
    TRACE("t_str_detail", tout << "internalizing term: " << mk_ismt2_pp(term, get_manager()) << std::endl;);
    unsigned num_args = term->get_num_args();
    expr* arg;
    for (unsigned i = 0; i < num_args; i++) {
        arg = term->get_arg(i);
        mk_var(ensure_enode(arg));
    }
    if (m.is_bool(term)) {
        bool_var bv = ctx.mk_bool_var(term);
        ctx.set_var_theory(bv, get_id());
        ctx.mark_as_relevant(bv);
    }

    enode* e = 0;
    if (ctx.e_internalized(term)) {
        e = ctx.get_enode(term);
    }
    else {
        e = ctx.mk_enode(term, false, m.is_bool(term), true);
    }

    if (opt_EagerStringConstantLengthAssertions && m_strutil.is_string(term)) {
        TRACE("t_str", tout << "eagerly asserting length of string term " << mk_pp(term, m) << std::endl;);
        m_basicstr_axiom_todo.insert(e);
        TRACE("t_str_axiom_bug", tout << "add " << mk_pp(e->get_owner(), m) << " to m_basicstr_axiom_todo" << std::endl;);
    }

    theory_var v = mk_var(e);
    TRACE("t_str_detail", tout << "term " << mk_ismt2_pp(term, get_manager()) << " = v#" << v << std::endl;);

    return true;
    */
}

enode* theory_str::ensure_enode(expr* e) {
    context& ctx = get_context();
    if (!ctx.e_internalized(e)) {
        ctx.internalize(e, false);
    }
    enode* n = ctx.get_enode(e);
    ctx.mark_as_relevant(n);
    return n;
}

void theory_str::refresh_theory_var(expr * e) {
    enode * en = ensure_enode(e);
    theory_var v = mk_var(en);
    TRACE("t_str_detail", tout << "refresh " << mk_pp(e, get_manager()) << ": v#" << v << std::endl;);
}

theory_var theory_str::mk_var(enode* n) {
    TRACE("t_str_detail", tout << "mk_var for " << mk_pp(n->get_owner(), get_manager()) << std::endl;);
    /*
    if (!m_strutil.is_string(n->get_owner())) {
        return null_theory_var;
    }
    */
    // TODO this may require an overhaul of m_strutil.is_string() if things suddenly start working after the following change:
    ast_manager & m = get_manager();
    if (!(is_sort_of(m.get_sort(n->get_owner()), m_strutil.get_fid(), STRING_SORT))) {
        return null_theory_var;
    }
    if (is_attached_to_var(n)) {
        TRACE("t_str_detail", tout << "already attached to theory var" << std::endl;);
        return n->get_th_var(get_id());
    } else {
        theory_var v = theory::mk_var(n);
        m_find.mk_var();
        TRACE("t_str_detail", tout << "new theory var v#" << v << std::endl;);
        get_context().attach_th_var(n, this, v);
        get_context().mark_as_relevant(n);
        return v;
    }
}

static void cut_vars_map_copy(std::map<expr*, int> & dest, std::map<expr*, int> & src) {
    std::map<expr*, int>::iterator itor = src.begin();
    for (; itor != src.end(); itor++) {
        dest[itor->first] = 1;
    }
}

bool theory_str::has_self_cut(expr * n1, expr * n2) {
	if (!cut_var_map.contains(n1)) {
        return false;
    }
    if (!cut_var_map.contains(n2)) {
        return false;
    }
    if (cut_var_map[n1].empty() || cut_var_map[n2].empty()) {
        return false;
    }

    std::map<expr*, int>::iterator itor = cut_var_map[n1].top()->vars.begin();
    for (; itor != cut_var_map[n1].top()->vars.end(); ++itor) {
        if (cut_var_map[n2].top()->vars.find(itor->first) != cut_var_map[n2].top()->vars.end()) {
            return true;
        }
    }
    return false;
}

void theory_str::add_cut_info_one_node(expr * baseNode, int slevel, expr * node) {
    // crash avoidance?
    m_trail.push_back(baseNode);
    m_trail.push_back(node);
    if (!cut_var_map.contains(baseNode)) {
        T_cut * varInfo = alloc(T_cut);
        varInfo->level = slevel;
        varInfo->vars[node] = 1;
        cut_var_map.insert(baseNode, std::stack<T_cut*>());
        cut_var_map[baseNode].push(varInfo);
        TRACE("t_str_cut_var_map", tout << "add var info for baseNode=" << mk_pp(baseNode, get_manager()) << ", node=" << mk_pp(node, get_manager()) << std::endl;);
    } else {
        if (cut_var_map[baseNode].empty()) {
            T_cut * varInfo = alloc(T_cut);
            varInfo->level = slevel;
            varInfo->vars[node] = 1;
            cut_var_map[baseNode].push(varInfo);
            TRACE("t_str_cut_var_map", tout << "add var info for baseNode=" << mk_pp(baseNode, get_manager()) << ", node=" << mk_pp(node, get_manager()) << std::endl;);
        } else {
            if (cut_var_map[baseNode].top()->level < slevel) {
                T_cut * varInfo = alloc(T_cut);
                varInfo->level = slevel;
                cut_vars_map_copy(varInfo->vars, cut_var_map[baseNode].top()->vars);
                varInfo->vars[node] = 1;
                cut_var_map[baseNode].push(varInfo);
                TRACE("t_str_cut_var_map", tout << "add var info for baseNode=" << mk_pp(baseNode, get_manager()) << ", node=" << mk_pp(node, get_manager()) << std::endl;);
            } else if (cut_var_map[baseNode].top()->level == slevel) {
                cut_var_map[baseNode].top()->vars[node] = 1;
                TRACE("t_str_cut_var_map", tout << "add var info for baseNode=" << mk_pp(baseNode, get_manager()) << ", node=" << mk_pp(node, get_manager()) << std::endl;);
            } else {
                get_manager().raise_exception("entered illegal state during add_cut_info_one_node()");
            }
        }
    }
}

void theory_str::add_cut_info_merge(expr * destNode, int slevel, expr * srcNode) {
    // crash avoidance?
    m_trail.push_back(destNode);
    m_trail.push_back(srcNode);
    if (!cut_var_map.contains(srcNode)) {
        get_manager().raise_exception("illegal state in add_cut_info_merge(): cut_var_map doesn't contain srcNode");
    }

    if (cut_var_map[srcNode].empty()) {
        get_manager().raise_exception("illegal state in add_cut_info_merge(): cut_var_map[srcNode] is empty");
    }

    if (!cut_var_map.contains(destNode)) {
        T_cut * varInfo = alloc(T_cut);
        varInfo->level = slevel;
        cut_vars_map_copy(varInfo->vars, cut_var_map[srcNode].top()->vars);
        cut_var_map.insert(destNode, std::stack<T_cut*>());
        cut_var_map[destNode].push(varInfo);
        TRACE("t_str_cut_var_map", tout << "merge var info for destNode=" << mk_pp(destNode, get_manager()) << ", srcNode=" << mk_pp(srcNode, get_manager()) << std::endl;);
    } else {
        if (cut_var_map[destNode].empty() || cut_var_map[destNode].top()->level < slevel) {
            T_cut * varInfo = alloc(T_cut);
            varInfo->level = slevel;
            cut_vars_map_copy(varInfo->vars, cut_var_map[destNode].top()->vars);
            cut_vars_map_copy(varInfo->vars, cut_var_map[srcNode].top()->vars);
            cut_var_map[destNode].push(varInfo);
            TRACE("t_str_cut_var_map", tout << "merge var info for destNode=" << mk_pp(destNode, get_manager()) << ", srcNode=" << mk_pp(srcNode, get_manager()) << std::endl;);
        } else if (cut_var_map[destNode].top()->level == slevel) {
            cut_vars_map_copy(cut_var_map[destNode].top()->vars, cut_var_map[srcNode].top()->vars);
            TRACE("t_str_cut_var_map", tout << "merge var info for destNode=" << mk_pp(destNode, get_manager()) << ", srcNode=" << mk_pp(srcNode, get_manager()) << std::endl;);
        } else {
            get_manager().raise_exception("illegal state in add_cut_info_merge(): inconsistent slevels");
        }
    }
}

void theory_str::check_and_init_cut_var(expr * node) {
    if (cut_var_map.contains(node)) {
        return;
    } else if (!m_strutil.is_string(node)) {
        add_cut_info_one_node(node, -1, node);
    }
}

literal theory_str::mk_literal(expr* _e) {
    ast_manager & m = get_manager();
    expr_ref e(_e, m);
    context& ctx = get_context();
    ensure_enode(e);
    return ctx.get_literal(e);
}

app * theory_str::mk_int(int n) {
    return m_autil.mk_numeral(rational(n), true);
}

app * theory_str::mk_int(rational & q) {
    return m_autil.mk_numeral(q, true);
}


// TODO refactor all of these so that they don't use variable counters, but use ast_manager::mk_fresh_const instead

expr * theory_str::mk_internal_lenTest_var(expr * node, int lTries) {
	ast_manager & m = get_manager();

	std::stringstream ss;
	ss << "$$_len_" << mk_ismt2_pp(node, m) << "_" << lTries << "_" << tmpLenTestVarCount;
	tmpLenTestVarCount += 1;
	std::string name = ss.str();
	app * var = mk_str_var(name);
	internal_lenTest_vars.insert(var);
	m_trail.push_back(var);
	return var;
}

expr * theory_str::mk_internal_valTest_var(expr * node, int len, int vTries) {
	ast_manager & m = get_manager();
	std::stringstream ss;
	ss << "$$_val_" << mk_ismt2_pp(node, m) << "_" << len << "_" << vTries << "_" << tmpValTestVarCount;
	tmpValTestVarCount += 1;
	std::string name = ss.str();
	app * var = mk_str_var(name);
	internal_valTest_vars.insert(var);
	m_trail.push_back(var);
	return var;
}

void theory_str::track_variable_scope(expr * var) {
    if (internal_variable_scope_levels.find(sLevel) == internal_variable_scope_levels.end()) {
        internal_variable_scope_levels[sLevel] = std::set<expr*>();
    }
    internal_variable_scope_levels[sLevel].insert(var);
}

app * theory_str::mk_internal_xor_var() {
    /*
	ast_manager & m = get_manager();
	std::stringstream ss;
	ss << tmpXorVarCount;
	tmpXorVarCount++;
	std::string name = "$$_xor_" + ss.str();
	// Z3_sort r = of_sort(mk_c(c)->m().mk_sort(mk_c(c)->get_arith_fid(), INT_SORT));
	sort * int_sort = m.mk_sort(m_autil.get_family_id(), INT_SORT);

	char * new_buffer = alloc_svect(char, name.length() + 1);
    strcpy(new_buffer, name.c_str());
	symbol sym(new_buffer);

	app * a = m.mk_const(m.mk_const_decl(sym, int_sort));
	m_trail.push_back(a);
	return a;
	*/
    return mk_int_var("$$_xor");
}

app * theory_str::mk_int_var(std::string name) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    TRACE("t_str_detail", tout << "creating integer variable " << name << " at scope level " << sLevel << std::endl;);

    sort * int_sort = m.mk_sort(m_autil.get_family_id(), INT_SORT);
    app * a = m.mk_fresh_const(name.c_str(), int_sort);

    ctx.internalize(a, false);
    SASSERT(ctx.get_enode(a) != NULL);
    SASSERT(ctx.e_internalized(a));
    ctx.mark_as_relevant(a);
    // I'm assuming that this combination will do the correct thing in the integer theory.

    //mk_var(ctx.get_enode(a));
    m_trail.push_back(a);
    //variable_set.insert(a);
    //internal_variable_set.insert(a);
    //track_variable_scope(a);

    return a;
}

app * theory_str::mk_unroll_bound_var() {
	return mk_int_var("unroll");
}

app * theory_str::mk_unroll_test_var() {
	app * v = mk_str_var("unrollTest"); // was uRt
	internal_unrollTest_vars.insert(v);
	track_variable_scope(v);
	return v;
}

app * theory_str::mk_str_var(std::string name) {
	context & ctx = get_context();
	ast_manager & m = get_manager();

	TRACE("t_str_detail", tout << "creating string variable " << name << " at scope level " << sLevel << std::endl;);

	sort * string_sort = m.mk_sort(get_family_id(), STRING_SORT);
	app * a = m.mk_fresh_const(name.c_str(), string_sort);

	TRACE("t_str_detail", tout << "a->get_family_id() = " << a->get_family_id() << std::endl
	        << "this->get_family_id() = " << this->get_family_id() << std::endl;);

	// I have a hunch that this may not get internalized for free...
	ctx.internalize(a, false);
	SASSERT(ctx.get_enode(a) != NULL);
	SASSERT(ctx.e_internalized(a));
	// this might help??
	mk_var(ctx.get_enode(a));
	m_basicstr_axiom_todo.push_back(ctx.get_enode(a));
	TRACE("t_str_axiom_bug", tout << "add " << mk_pp(a, m) << " to m_basicstr_axiom_todo" << std::endl;);

	m_trail.push_back(a);
	variable_set.insert(a);
	internal_variable_set.insert(a);
	track_variable_scope(a);

	return a;
}

app * theory_str::mk_regex_rep_var() {
	context & ctx = get_context();
	ast_manager & m = get_manager();

	sort * string_sort = m.mk_sort(get_family_id(), STRING_SORT);
	app * a = m.mk_fresh_const("regex", string_sort);

	ctx.internalize(a, false);
	SASSERT(ctx.get_enode(a) != NULL);
	SASSERT(ctx.e_internalized(a));
	mk_var(ctx.get_enode(a));
	m_basicstr_axiom_todo.push_back(ctx.get_enode(a));
	TRACE("t_str_axiom_bug", tout << "add " << mk_pp(a, m) << " to m_basicstr_axiom_todo" << std::endl;);

	m_trail.push_back(a);
	// TODO cross-check which variable sets we need
	variable_set.insert(a);
	//internal_variable_set.insert(a);
	regex_variable_set.insert(a);
	track_variable_scope(a);

	return a;
}

app * theory_str::mk_nonempty_str_var() {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    std::stringstream ss;
    ss << tmpStringVarCount;
    tmpStringVarCount++;
    std::string name = "$$_str" + ss.str();

    TRACE("t_str_detail", tout << "creating nonempty string variable " << name << " at scope level " << sLevel << std::endl;);

    sort * string_sort = m.mk_sort(get_family_id(), STRING_SORT);
    app * a = m.mk_fresh_const(name.c_str(), string_sort);

    ctx.internalize(a, false);
    SASSERT(ctx.get_enode(a) != NULL);
    // this might help??
    mk_var(ctx.get_enode(a));

    // assert a variation of the basic string axioms that ensures this string is nonempty
    {
        // build LHS
        expr * len_str = mk_strlen(a);
        SASSERT(len_str);
        // build RHS
        expr * zero = m_autil.mk_numeral(rational(0), true);
        SASSERT(zero);
        // build LHS > RHS and assert
        // we have to build !(LHS <= RHS) instead
        app * lhs_gt_rhs = m.mk_not(m_autil.mk_le(len_str, zero));
        SASSERT(lhs_gt_rhs);
        assert_axiom(lhs_gt_rhs);
    }

    // add 'a' to variable sets, so we can keep track of it
    m_trail.push_back(a);
    variable_set.insert(a);
    internal_variable_set.insert(a);
    track_variable_scope(a);

    return a;
}

app * theory_str::mk_unroll(expr * n, expr * bound) {
	context & ctx = get_context();
	ast_manager & m = get_manager();

	expr * args[2] = {n, bound};
	app * unrollFunc = get_manager().mk_app(get_id(), OP_RE_UNROLL, 0, 0, 2, args);

	expr_ref_vector items(m);
	items.push_back(ctx.mk_eq_atom(ctx.mk_eq_atom(bound, mk_int(0)), ctx.mk_eq_atom(unrollFunc, m_strutil.mk_string(""))));
	items.push_back(m_autil.mk_ge(bound, mk_int(0)));
	items.push_back(m_autil.mk_ge(mk_strlen(unrollFunc), mk_int(0)));

	expr_ref finalAxiom(mk_and(items), m);
	SASSERT(finalAxiom);
	assert_axiom(finalAxiom);
	return unrollFunc;
}

app * theory_str::mk_contains(expr * haystack, expr * needle) {
    expr * args[2] = {haystack, needle};
    app * contains = get_manager().mk_app(get_id(), OP_STR_CONTAINS, 0, 0, 2, args);
    // immediately force internalization so that axiom setup does not fail
    get_context().internalize(contains, false);
    set_up_axioms(contains);
    return contains;
}

app * theory_str::mk_indexof(expr * haystack, expr * needle) {
    expr * args[2] = {haystack, needle};
    app * indexof = get_manager().mk_app(get_id(), OP_STR_INDEXOF, 0, 0, 2, args);
    // immediately force internalization so that axiom setup does not fail
    get_context().internalize(indexof, false);
    set_up_axioms(indexof);
    return indexof;
}

app * theory_str::mk_strlen(expr * e) {
    /*if (m_strutil.is_string(e)) {*/ if (false) {
        const char * strval = 0;
        m_strutil.is_string(e, &strval);
        int len = strlen(strval);
        return m_autil.mk_numeral(rational(len), true);
    } else {
        if (false) {
            // use cache
            app * lenTerm = NULL;
            if (!length_ast_map.find(e, lenTerm)) {
                expr * args[1] = {e};
                lenTerm = get_manager().mk_app(get_id(), OP_STRLEN, 0, 0, 1, args);
                length_ast_map.insert(e, lenTerm);
                m_trail.push_back(lenTerm);
            }
            return lenTerm;
        } else {
            // always regen
            expr * args[1] = {e};
            return get_manager().mk_app(get_id(), OP_STRLEN, 0, 0, 1, args);
        }
    }
}

/*
 * Returns the simplified concatenation of two expressions,
 * where either both expressions are constant strings
 * or one expression is the empty string.
 * If this precondition does not hold, the function returns NULL.
 * (note: this function was strTheory::Concat())
 */
expr * theory_str::mk_concat_const_str(expr * n1, expr * n2) {
    bool n1HasEqcValue = false;
    bool n2HasEqcValue = false;
    expr * v1 = get_eqc_value(n1, n1HasEqcValue);
    expr * v2 = get_eqc_value(n2, n2HasEqcValue);
    if (n1HasEqcValue && n2HasEqcValue) {
        const char * n1_str_tmp;
        m_strutil.is_string(v1, & n1_str_tmp);
        std::string n1_str(n1_str_tmp);
        const char * n2_str_tmp;
        m_strutil.is_string(v2, & n2_str_tmp);
        std::string n2_str(n2_str_tmp);
        std::string result = n1_str + n2_str;
        return m_strutil.mk_string(result);
    } else if (n1HasEqcValue && !n2HasEqcValue) {
        const char * n1_str_tmp;
        m_strutil.is_string(v1, & n1_str_tmp);
        if (strcmp(n1_str_tmp, "") == 0) {
            return n2;
        }
    } else if (!n1HasEqcValue && n2HasEqcValue) {
        const char * n2_str_tmp;
        m_strutil.is_string(v2, & n2_str_tmp);
        if (strcmp(n2_str_tmp, "") == 0) {
            return n1;
        }
    }
    return NULL;
}

expr * theory_str::mk_concat(expr * n1, expr * n2) {
    context & ctx = get_context();
	ast_manager & m = get_manager();
	ENSURE(n1 != NULL);
	ENSURE(n2 != NULL);
	bool n1HasEqcValue = false;
	bool n2HasEqcValue = false;
	n1 = get_eqc_value(n1, n1HasEqcValue);
	n2 = get_eqc_value(n2, n2HasEqcValue);
	if (n1HasEqcValue && n2HasEqcValue) {
	    return mk_concat_const_str(n1, n2);
	} else if (n1HasEqcValue && !n2HasEqcValue) {
	    bool n2_isConcatFunc = is_concat(to_app(n2));
	    if (m_strutil.get_string_constant_value(n1) == "") {
	        return n2;
	    }
	    if (n2_isConcatFunc) {
	        expr * n2_arg0 = to_app(n2)->get_arg(0);
	        expr * n2_arg1 = to_app(n2)->get_arg(1);
	        if (m_strutil.is_string(n2_arg0)) {
	            n1 = mk_concat_const_str(n1, n2_arg0); // n1 will be a constant
	            n2 = n2_arg1;
	        }
	    }
	} else if (!n1HasEqcValue && n2HasEqcValue) {
	    if (m_strutil.get_string_constant_value(n2) == "") {
	        return n1;
	    }

	    if (is_concat(to_app(n1))) {
	        expr * n1_arg0 = to_app(n1)->get_arg(0);
	        expr * n1_arg1 = to_app(n1)->get_arg(1);
	        if (m_strutil.is_string(n1_arg1)) {
	            n1 = n1_arg0;
	            n2 = mk_concat_const_str(n1_arg1, n2); // n2 will be a constant
	        }
	    }
	} else {
	    if (is_concat(to_app(n1)) && is_concat(to_app(n2))) {
	        expr * n1_arg0 = to_app(n1)->get_arg(0);
	        expr * n1_arg1 = to_app(n1)->get_arg(1);
	        expr * n2_arg0 = to_app(n2)->get_arg(0);
	        expr * n2_arg1 = to_app(n2)->get_arg(1);
	        if (m_strutil.is_string(n1_arg1) && m_strutil.is_string(n2_arg0)) {
	            expr * tmpN1 = n1_arg0;
	            expr * tmpN2 = mk_concat_const_str(n1_arg1, n2_arg0);
	            n1 = mk_concat(tmpN1, tmpN2);
	            n2 = n2_arg1;
	        }
	    }
	}

	//------------------------------------------------------
	// * expr * ast1 = mk_2_arg_app(ctx, td->Concat, n1, n2);
	// * expr * ast2 = mk_2_arg_app(ctx, td->Concat, n1, n2);
	// Z3 treats (ast1) and (ast2) as two different nodes.
	//-------------------------------------------------------

	expr * concatAst = NULL;

	if (!concat_astNode_map.find(n1, n2, concatAst)) {
	    expr * args[2] = {n1, n2};
	    concatAst = m.mk_app(get_id(), OP_STRCAT, 0, 0, 2, args);
	    m_trail.push_back(concatAst);
	    concat_astNode_map.insert(n1, n2, concatAst);

	    expr_ref concat_length(mk_strlen(concatAst), m);

	    ptr_vector<expr> childrenVector;
	    get_nodes_in_concat(concatAst, childrenVector);
	    expr_ref_vector items(m);
	    for (unsigned int i = 0; i < childrenVector.size(); i++) {
	        items.push_back(mk_strlen(childrenVector.get(i)));
	    }
	    expr_ref lenAssert(ctx.mk_eq_atom(concat_length, m_autil.mk_add(items.size(), items.c_ptr())), m);
	    assert_axiom(lenAssert);
	}
	return concatAst;
}

bool theory_str::can_propagate() {
    return !m_basicstr_axiom_todo.empty() || !m_str_eq_todo.empty() || !m_concat_axiom_todo.empty() || !m_concat_eval_todo.empty()
            || !m_axiom_CharAt_todo.empty() || !m_axiom_StartsWith_todo.empty() || !m_axiom_EndsWith_todo.empty()
            || !m_axiom_Contains_todo.empty() || !m_axiom_Indexof_todo.empty() || !m_axiom_Indexof2_todo.empty() || !m_axiom_LastIndexof_todo.empty()
            || !m_axiom_Substr_todo.empty() || !m_axiom_Replace_todo.empty()
			|| !m_axiom_RegexIn_todo.empty()
			|| !m_delayed_axiom_setup_terms.empty();
            ;
}

void theory_str::propagate() {
    context & ctx = get_context();
    while (can_propagate()) {
        TRACE("t_str_detail", tout << "propagating..." << std::endl;);
        for (unsigned i = 0; i < m_basicstr_axiom_todo.size(); ++i) {
            instantiate_basic_string_axioms(m_basicstr_axiom_todo[i]);
        }
        m_basicstr_axiom_todo.reset();
        TRACE("t_str_axiom_bug", tout << "reset m_basicstr_axiom_todo" << std::endl;);

        for (unsigned i = 0; i < m_str_eq_todo.size(); ++i) {
            std::pair<enode*,enode*> pair = m_str_eq_todo[i];
            enode * lhs = pair.first;
            enode * rhs = pair.second;
            handle_equality(lhs->get_owner(), rhs->get_owner());
        }
        m_str_eq_todo.reset();

        for (unsigned i = 0; i < m_concat_axiom_todo.size(); ++i) {
            instantiate_concat_axiom(m_concat_axiom_todo[i]);
        }
        m_concat_axiom_todo.reset();

        for (unsigned i = 0; i < m_concat_eval_todo.size(); ++i) {
            try_eval_concat(m_concat_eval_todo[i]);
        }
        m_concat_eval_todo.reset();

        for (unsigned i = 0; i < m_axiom_CharAt_todo.size(); ++i) {
            instantiate_axiom_CharAt(m_axiom_CharAt_todo[i]);
        }
        m_axiom_CharAt_todo.reset();

        for (unsigned i = 0; i < m_axiom_StartsWith_todo.size(); ++i) {
            instantiate_axiom_StartsWith(m_axiom_StartsWith_todo[i]);
        }
        m_axiom_StartsWith_todo.reset();

        for (unsigned i = 0; i < m_axiom_EndsWith_todo.size(); ++i) {
            instantiate_axiom_EndsWith(m_axiom_EndsWith_todo[i]);
        }
        m_axiom_EndsWith_todo.reset();

        for (unsigned i = 0; i < m_axiom_Contains_todo.size(); ++i) {
            instantiate_axiom_Contains(m_axiom_Contains_todo[i]);
        }
        m_axiom_Contains_todo.reset();

        for (unsigned i = 0; i < m_axiom_Indexof_todo.size(); ++i) {
            instantiate_axiom_Indexof(m_axiom_Indexof_todo[i]);
        }
        m_axiom_Indexof_todo.reset();

        for (unsigned i = 0; i < m_axiom_Indexof2_todo.size(); ++i) {
            instantiate_axiom_Indexof2(m_axiom_Indexof2_todo[i]);
        }
        m_axiom_Indexof2_todo.reset();

        for (unsigned i = 0; i < m_axiom_LastIndexof_todo.size(); ++i) {
            instantiate_axiom_LastIndexof(m_axiom_LastIndexof_todo[i]);
        }
        m_axiom_LastIndexof_todo.reset();

        for (unsigned i = 0; i < m_axiom_Substr_todo.size(); ++i) {
            instantiate_axiom_Substr(m_axiom_Substr_todo[i]);
        }
        m_axiom_Substr_todo.reset();

        for (unsigned i = 0; i < m_axiom_Replace_todo.size(); ++i) {
            instantiate_axiom_Replace(m_axiom_Replace_todo[i]);
        }
        m_axiom_Replace_todo.reset();

        for (unsigned i = 0; i < m_axiom_RegexIn_todo.size(); ++i) {
        	instantiate_axiom_RegexIn(m_axiom_RegexIn_todo[i]);
        }
        m_axiom_RegexIn_todo.reset();

        for (unsigned i = 0; i < m_delayed_axiom_setup_terms.size(); ++i) {
            // I think this is okay
            ctx.internalize(m_delayed_axiom_setup_terms[i].get(), false);
            set_up_axioms(m_delayed_axiom_setup_terms[i].get());
        }
        m_delayed_axiom_setup_terms.reset();
    }
}

/*
 * Attempt to evaluate a concat over constant strings,
 * and if this is possible, assert equality between the
 * flattened string and the original term.
 */

void theory_str::try_eval_concat(enode * cat) {
    SASSERT(is_concat(cat));
    app * a_cat = cat->get_owner();

    context & ctx = get_context();
    ast_manager & m = get_manager();

    TRACE("t_str_detail", tout << "attempting to flatten " << mk_pp(a_cat, m) << std::endl;);

    std::stack<app*> worklist;
    std::string flattenedString("");
    bool constOK = true;

    {
        app * arg0 = to_app(a_cat->get_arg(0));
        app * arg1 = to_app(a_cat->get_arg(1));

        worklist.push(arg1);
        worklist.push(arg0);
    }

    while (constOK && !worklist.empty()) {
        app * evalArg = worklist.top(); worklist.pop();
        if (m_strutil.is_string(evalArg)) {
            std::string nextStr = m_strutil.get_string_constant_value(evalArg);
            flattenedString.append(nextStr);
        } else if (is_concat(evalArg)) {
            app * arg0 = to_app(evalArg->get_arg(0));
            app * arg1 = to_app(evalArg->get_arg(1));

            worklist.push(arg1);
            worklist.push(arg0);
        } else {
            TRACE("t_str_detail", tout << "non-constant term in concat -- giving up." << std::endl;);
            constOK = false;
            break;
        }
    }
    if (constOK) {
        TRACE("t_str_detail", tout << "flattened to \"" << flattenedString << "\"" << std::endl;);
        expr_ref constStr(m_strutil.mk_string(flattenedString), m);
        expr_ref axiom(ctx.mk_eq_atom(a_cat, constStr), m);
        assert_axiom(axiom);
    }
}

/*
 * Instantiate an axiom of the following form:
 * Length(Concat(x, y)) = Length(x) + Length(y)
 */
void theory_str::instantiate_concat_axiom(enode * cat) {
    SASSERT(is_concat(cat));
    app * a_cat = cat->get_owner();

    ast_manager & m = get_manager();

    TRACE("t_str_detail", tout << "instantiating concat axiom for " << mk_ismt2_pp(a_cat, m) << std::endl;);

    // build LHS
    expr_ref len_xy(m);
    // TODO should we use str_util for these and other expressions?
    len_xy = mk_strlen(a_cat);
    SASSERT(len_xy);

    // build RHS: start by extracting x and y from Concat(x, y)
    unsigned nArgs = a_cat->get_num_args();
    SASSERT(nArgs == 2);
    app * a_x = to_app(a_cat->get_arg(0));
    app * a_y = to_app(a_cat->get_arg(1));

    expr_ref len_x(m);
    len_x = mk_strlen(a_x);
    SASSERT(len_x);

    expr_ref len_y(m);
    len_y = mk_strlen(a_y);
    SASSERT(len_y);

    // now build len_x + len_y
    expr_ref len_x_plus_len_y(m);
    len_x_plus_len_y = m_autil.mk_add(len_x, len_y);
    SASSERT(len_x_plus_len_y);

    // finally assert equality between the two subexpressions
    app * eq = m.mk_eq(len_xy, len_x_plus_len_y);
    SASSERT(eq);
    assert_axiom(eq);
}

/*
 * Add axioms that are true for any string variable:
 * 1. Length(x) >= 0
 * 2. Length(x) == 0 <=> x == ""
 * If the term is a string constant, we can assert something stronger:
 *    Length(x) == strlen(x)
 */
void theory_str::instantiate_basic_string_axioms(enode * str) {
    // TODO keep track of which enodes we have added axioms for, so we don't add the same ones twice?

    context & ctx = get_context();
    ast_manager & m = get_manager();

    TRACE("t_str_axiom_bug", tout << "set up basic string axioms on " << mk_pp(str->get_owner(), m) << std::endl;);

    // TESTING: attempt to avoid a crash here when a variable goes out of scope
    // TODO this seems to work so we probably need to do this for other propagate checks, etc.
    if (str->get_iscope_lvl() > ctx.get_scope_level()) {
        TRACE("t_str_detail", tout << "WARNING: skipping axiom setup on out-of-scope string term" << std::endl;);
        return;
    }

    // generate a stronger axiom for constant strings
    app * a_str = str->get_owner();
    if (m_strutil.is_string(str->get_owner())) {
        expr_ref len_str(m);
        len_str = mk_strlen(a_str);
        SASSERT(len_str);

        const char * strconst = 0;
        m_strutil.is_string(str->get_owner(), & strconst);
        TRACE("t_str_detail", tout << "instantiating constant string axioms for \"" << strconst << "\"" << std::endl;);
        int l = strlen(strconst);
        expr_ref len(m_autil.mk_numeral(rational(l), true), m);

        literal lit(mk_eq(len_str, len, false));
        ctx.mark_as_relevant(lit);
        ctx.mk_th_axiom(get_id(), 1, &lit);
    } else {
        // build axiom 1: Length(a_str) >= 0
        {
            // build LHS
            expr_ref len_str(m);
            len_str = mk_strlen(a_str);
            SASSERT(len_str);
            // build RHS
            expr_ref zero(m);
            zero = m_autil.mk_numeral(rational(0), true);
            SASSERT(zero);
            // build LHS >= RHS and assert
            app * lhs_ge_rhs = m_autil.mk_ge(len_str, zero);
            SASSERT(lhs_ge_rhs);
            TRACE("t_str_detail", tout << "string axiom 1: " << mk_ismt2_pp(lhs_ge_rhs, m) << std::endl;);
            assert_axiom(lhs_ge_rhs);
        }

        // build axiom 2: Length(a_str) == 0 <=> a_str == ""
        {
            // build LHS of iff
            expr_ref len_str(m);
            len_str = mk_strlen(a_str);
            SASSERT(len_str);
            expr_ref zero(m);
            zero = m_autil.mk_numeral(rational(0), true);
            SASSERT(zero);
            expr_ref lhs(m);
            lhs = ctx.mk_eq_atom(len_str, zero);
            SASSERT(lhs);
            // build RHS of iff
            expr_ref empty_str(m);
            empty_str = m_strutil.mk_string("");
            SASSERT(empty_str);
            expr_ref rhs(m);
            rhs = ctx.mk_eq_atom(a_str, empty_str);
            SASSERT(rhs);
            // build LHS <=> RHS and assert
            TRACE("t_str_detail", tout << "string axiom 2: " << mk_ismt2_pp(lhs, m) << " <=> " << mk_ismt2_pp(rhs, m) << std::endl;);
            literal l(mk_eq(lhs, rhs, true));
            ctx.mark_as_relevant(l);
            ctx.mk_th_axiom(get_id(), 1, &l);
        }

    }
}

/*
 * Add an axiom of the form:
 * (lhs == rhs) -> ( Length(lhs) == Length(rhs) )
 */
void theory_str::instantiate_str_eq_length_axiom(enode * lhs, enode * rhs) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    app * a_lhs = lhs->get_owner();
    app * a_rhs = rhs->get_owner();

    // build premise: (lhs == rhs)
    expr_ref premise(ctx.mk_eq_atom(a_lhs, a_rhs), m);

    // build conclusion: ( Length(lhs) == Length(rhs) )
    expr_ref len_lhs(mk_strlen(a_lhs), m);
    SASSERT(len_lhs);
    expr_ref len_rhs(mk_strlen(a_rhs), m);
    SASSERT(len_rhs);
    expr_ref conclusion(ctx.mk_eq_atom(len_lhs, len_rhs), m);

    TRACE("t_str_detail", tout << "string-eq length-eq axiom: "
            << mk_ismt2_pp(premise, m) << " -> " << mk_ismt2_pp(conclusion, m) << std::endl;);
    assert_implication(premise, conclusion);
}

void theory_str::instantiate_axiom_CharAt(enode * e) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    app * expr = e->get_owner();
    if (axiomatized_terms.contains(expr)) {
        TRACE("t_str_detail", tout << "already set up CharAt axiom for " << mk_pp(expr, m) << std::endl;);
        return;
    }
    axiomatized_terms.insert(expr);

    TRACE("t_str_detail", tout << "instantiate CharAt axiom for " << mk_pp(expr, m) << std::endl;);

    expr_ref ts0(mk_str_var("ts0"), m);
    expr_ref ts1(mk_str_var("ts1"), m);
    expr_ref ts2(mk_str_var("ts2"), m);

    expr_ref cond(m.mk_and(
            m_autil.mk_ge(expr->get_arg(1), mk_int(0)),
            // REWRITE for arithmetic theory:
            // m_autil.mk_lt(expr->get_arg(1), mk_strlen(expr->get_arg(0)))
            m.mk_not(m_autil.mk_ge(m_autil.mk_add(expr->get_arg(1), m_autil.mk_mul(mk_int(-1), mk_strlen(expr->get_arg(0)))), mk_int(0)))
            ), m);

    expr_ref_vector and_item(m);
    and_item.push_back(ctx.mk_eq_atom(expr->get_arg(0), mk_concat(ts0, mk_concat(ts1, ts2))));
    and_item.push_back(ctx.mk_eq_atom(expr->get_arg(1), mk_strlen(ts0)));
    and_item.push_back(ctx.mk_eq_atom(mk_strlen(ts1), mk_int(1)));

    expr_ref thenBranch(m.mk_and(and_item.size(), and_item.c_ptr()), m);
    expr_ref elseBranch(ctx.mk_eq_atom(ts1, m_strutil.mk_string("")), m);

    expr_ref axiom(m.mk_ite(cond, thenBranch, elseBranch), m);
    expr_ref reductionVar(ctx.mk_eq_atom(expr, ts1), m);

    SASSERT(axiom);
    SASSERT(reductionVar);

    expr_ref finalAxiom(m.mk_and(axiom, reductionVar), m);
    SASSERT(finalAxiom);
    assert_axiom(finalAxiom);
}

void theory_str::instantiate_axiom_StartsWith(enode * e) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    app * expr = e->get_owner();
    if (axiomatized_terms.contains(expr)) {
        TRACE("t_str_detail", tout << "already set up StartsWith axiom for " << mk_pp(expr, m) << std::endl;);
        return;
    }
    axiomatized_terms.insert(expr);

    TRACE("t_str_detail", tout << "instantiate StartsWith axiom for " << mk_pp(expr, m) << std::endl;);

    expr_ref ts0(mk_str_var("ts0"), m);
    expr_ref ts1(mk_str_var("ts1"), m);

    expr_ref_vector innerItems(m);
    innerItems.push_back(ctx.mk_eq_atom(expr->get_arg(0), mk_concat(ts0, ts1)));
    innerItems.push_back(ctx.mk_eq_atom(mk_strlen(ts0), mk_strlen(expr->get_arg(1))));
    innerItems.push_back(m.mk_ite(ctx.mk_eq_atom(ts0, expr->get_arg(1)), expr, m.mk_not(expr)));
    expr_ref then1(m.mk_and(innerItems.size(), innerItems.c_ptr()), m);
    SASSERT(then1);

    // the top-level condition is Length(arg0) >= Length(arg1).
    // of course, the integer theory is not so accommodating
    expr_ref topLevelCond(
            m_autil.mk_ge(
                    m_autil.mk_add(
                            mk_strlen(expr->get_arg(0)), m_autil.mk_mul(mk_int(-1), mk_strlen(expr->get_arg(1)))),
                    mk_int(0))
            , m);
    SASSERT(topLevelCond);

    expr_ref finalAxiom(m.mk_ite(topLevelCond, then1, m.mk_not(expr)), m);
    SASSERT(finalAxiom);
    assert_axiom(finalAxiom);
}

void theory_str::instantiate_axiom_EndsWith(enode * e) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    app * expr = e->get_owner();
    if (axiomatized_terms.contains(expr)) {
        TRACE("t_str_detail", tout << "already set up EndsWith axiom for " << mk_pp(expr, m) << std::endl;);
        return;
    }
    axiomatized_terms.insert(expr);

    TRACE("t_str_detail", tout << "instantiate EndsWith axiom for " << mk_pp(expr, m) << std::endl;);

    expr_ref ts0(mk_str_var("ts0"), m);
    expr_ref ts1(mk_str_var("ts1"), m);

    expr_ref_vector innerItems(m);
    innerItems.push_back(ctx.mk_eq_atom(expr->get_arg(0), mk_concat(ts0, ts1)));
    innerItems.push_back(ctx.mk_eq_atom(mk_strlen(ts1), mk_strlen(expr->get_arg(1))));
    innerItems.push_back(m.mk_ite(ctx.mk_eq_atom(ts1, expr->get_arg(1)), expr, m.mk_not(expr)));
    expr_ref then1(m.mk_and(innerItems.size(), innerItems.c_ptr()), m);
    SASSERT(then1);

    // the top-level condition is Length(arg0) >= Length(arg1)
    expr_ref topLevelCond(
            m_autil.mk_ge(
                    m_autil.mk_add(
                            mk_strlen(expr->get_arg(0)), m_autil.mk_mul(mk_int(-1), mk_strlen(expr->get_arg(1)))),
                    mk_int(0))
            , m);
    SASSERT(topLevelCond);

    expr_ref finalAxiom(m.mk_ite(topLevelCond, then1, m.mk_not(expr)), m);
    SASSERT(finalAxiom);
    assert_axiom(finalAxiom);
}

void theory_str::instantiate_axiom_Contains(enode * e) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    app * ex = e->get_owner();
    if (axiomatized_terms.contains(ex)) {
        TRACE("t_str_detail", tout << "already set up Contains axiom for " << mk_pp(ex, m) << std::endl;);
        return;
    }
    axiomatized_terms.insert(ex);

    // quick path, because this is necessary due to rewriter behaviour
    // (at minimum it should fix z3str/concat-006.smt2
    // TODO: see if it's necessary for other such terms
    if (m_strutil.is_string(ex->get_arg(0)) && m_strutil.is_string(ex->get_arg(1))) {
        TRACE("t_str_detail", tout << "eval constant Contains term " << mk_pp(ex, m) << std::endl;);
        std::string haystackStr = m_strutil.get_string_constant_value(ex->get_arg(0));
        std::string needleStr = m_strutil.get_string_constant_value(ex->get_arg(1));
        if (haystackStr.find(needleStr) != std::string::npos) {
            assert_axiom(ex);
        } else {
            assert_axiom(m.mk_not(ex));
        }
        return;
    }

    { // register Contains()
        expr * str = ex->get_arg(0);
        expr * substr = ex->get_arg(1);
        contains_map.push_back(ex);
        std::pair<expr*, expr*> key = std::pair<expr*, expr*>(str, substr);
        contain_pair_bool_map.insert(str, substr, ex);
        contain_pair_idx_map[str].insert(key);
        contain_pair_idx_map[substr].insert(key);
    }

    TRACE("t_str_detail", tout << "instantiate Contains axiom for " << mk_pp(ex, m) << std::endl;);

    expr_ref ts0(mk_str_var("ts0"), m);
    expr_ref ts1(mk_str_var("ts1"), m);

    expr_ref breakdownAssert(ctx.mk_eq_atom(ex, ctx.mk_eq_atom(ex->get_arg(0), mk_concat(ts0, mk_concat(ex->get_arg(1), ts1)))), m);
    SASSERT(breakdownAssert);
    assert_axiom(breakdownAssert);
}

void theory_str::instantiate_axiom_Indexof(enode * e) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    app * expr = e->get_owner();
    if (axiomatized_terms.contains(expr)) {
        TRACE("t_str_detail", tout << "already set up Indexof axiom for " << mk_pp(expr, m) << std::endl;);
        return;
    }
    axiomatized_terms.insert(expr);

    TRACE("t_str_detail", tout << "instantiate Indexof axiom for " << mk_pp(expr, m) << std::endl;);

    expr_ref x1(mk_str_var("x1"), m);
    expr_ref x2(mk_str_var("x2"), m);
    expr_ref indexAst(mk_int_var("index"), m);

    expr_ref condAst(mk_contains(expr->get_arg(0), expr->get_arg(1)), m);
    SASSERT(condAst);

    // -----------------------
    // true branch
    expr_ref_vector thenItems(m);
    //  args[0] = x1 . args[1] . x2
    thenItems.push_back(ctx.mk_eq_atom(expr->get_arg(0), mk_concat(x1, mk_concat(expr->get_arg(1), x2))));
    //  indexAst = |x1|
    thenItems.push_back(ctx.mk_eq_atom(indexAst, mk_strlen(x1)));
    //     args[0]  = x3 . x4
    //  /\ |x3| = |x1| + |args[1]| - 1
    //  /\ ! contains(x3, args[1])
    expr_ref x3(mk_str_var("x3"), m);
    expr_ref x4(mk_str_var("x4"), m);
    expr_ref tmpLen(m_autil.mk_add(indexAst, mk_strlen(expr->get_arg(1)), mk_int(-1)), m);
    SASSERT(tmpLen);
    thenItems.push_back(ctx.mk_eq_atom(expr->get_arg(0), mk_concat(x3, x4)));
    thenItems.push_back(ctx.mk_eq_atom(mk_strlen(x3), tmpLen));
    thenItems.push_back(m.mk_not(mk_contains(x3, expr->get_arg(1))));
    expr_ref thenBranch(m.mk_and(thenItems.size(), thenItems.c_ptr()), m);
    SASSERT(thenBranch);

    // -----------------------
    // false branch
    expr_ref elseBranch(ctx.mk_eq_atom(indexAst, mk_int(-1)), m);
    SASSERT(elseBranch);

    expr_ref breakdownAssert(m.mk_ite(condAst, thenBranch, elseBranch), m);
    SASSERT(breakdownAssert);

    expr_ref reduceToIndex(ctx.mk_eq_atom(expr, indexAst), m);
    SASSERT(reduceToIndex);

    expr_ref finalAxiom(m.mk_and(breakdownAssert, reduceToIndex), m);
    SASSERT(finalAxiom);
    assert_axiom(finalAxiom);
}

void theory_str::instantiate_axiom_Indexof2(enode * e) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    app * expr = e->get_owner();
    if (axiomatized_terms.contains(expr)) {
        TRACE("t_str_detail", tout << "already set up Indexof2 axiom for " << mk_pp(expr, m) << std::endl;);
        return;
    }
    axiomatized_terms.insert(expr);

    TRACE("t_str_detail", tout << "instantiate Indexof2 axiom for " << mk_pp(expr, m) << std::endl;);

    // -------------------------------------------------------------------------------
    //   if (arg[2] >= length(arg[0]))                          // ite2
    //     resAst = -1
    //   else
    //     args[0] = prefix . suffix
    //     /\ indexAst = indexof(suffix, arg[1])
    //     /\ args[2] = len(prefix)
    //     /\ if (indexAst == -1)  resAst = indexAst            // ite3
    //        else   resAst = args[2] + indexAst
    // -------------------------------------------------------------------------------

    expr_ref resAst(mk_int_var("res"), m);
    expr_ref indexAst(mk_int_var("index"), m);
    expr_ref prefix(mk_str_var("prefix"), m);
    expr_ref suffix(mk_str_var("suffix"), m);
    expr_ref prefixLen(mk_strlen(prefix), m);
    expr_ref zeroAst(mk_int(0), m);
    expr_ref negOneAst(mk_int(-1), m);

    expr_ref ite3(m.mk_ite(
            ctx.mk_eq_atom(indexAst, negOneAst),
            ctx.mk_eq_atom(resAst, negOneAst),
            ctx.mk_eq_atom(resAst, m_autil.mk_add(expr->get_arg(2), indexAst))
            ),m);

    expr_ref_vector ite2ElseItems(m);
    ite2ElseItems.push_back(ctx.mk_eq_atom(expr->get_arg(0), mk_concat(prefix, suffix)));
    ite2ElseItems.push_back(ctx.mk_eq_atom(indexAst, mk_indexof(suffix, expr->get_arg(1))));
    ite2ElseItems.push_back(ctx.mk_eq_atom(expr->get_arg(2), prefixLen));
    ite2ElseItems.push_back(ite3);
    expr_ref ite2Else(m.mk_and(ite2ElseItems.size(), ite2ElseItems.c_ptr()), m);
    SASSERT(ite2Else);

    expr_ref ite2(m.mk_ite(
            //m_autil.mk_ge(expr->get_arg(2), mk_strlen(expr->get_arg(0))),
            m_autil.mk_ge(m_autil.mk_add(expr->get_arg(2), m_autil.mk_mul(mk_int(-1), mk_strlen(expr->get_arg(0)))), zeroAst),
            ctx.mk_eq_atom(resAst, negOneAst),
            ite2Else
            ), m);
    SASSERT(ite2);

    expr_ref ite1(m.mk_ite(
            //m_autil.mk_lt(expr->get_arg(2), zeroAst),
            m.mk_not(m_autil.mk_ge(expr->get_arg(2), zeroAst)),
            ctx.mk_eq_atom(resAst, mk_indexof(expr->get_arg(0), expr->get_arg(1))),
            ite2
            ), m);
    SASSERT(ite1);
    assert_axiom(ite1);

    expr_ref reduceTerm(ctx.mk_eq_atom(expr, resAst), m);
    SASSERT(reduceTerm);
    assert_axiom(reduceTerm);
}

void theory_str::instantiate_axiom_LastIndexof(enode * e) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    app * expr = e->get_owner();
    if (axiomatized_terms.contains(expr)) {
        TRACE("t_str_detail", tout << "already set up LastIndexof axiom for " << mk_pp(expr, m) << std::endl;);
        return;
    }
    axiomatized_terms.insert(expr);

    TRACE("t_str_detail", tout << "instantiate LastIndexof axiom for " << mk_pp(expr, m) << std::endl;);

    expr_ref x1(mk_str_var("x1"), m);
    expr_ref x2(mk_str_var("x2"), m);
    expr_ref indexAst(mk_int_var("index"), m);
    expr_ref_vector items(m);

    // args[0] = x1 . args[1] . x2
    expr_ref eq1(ctx.mk_eq_atom(expr->get_arg(0), mk_concat(x1, mk_concat(expr->get_arg(1), x2))), m);
    expr_ref arg0HasArg1(mk_contains(expr->get_arg(0), expr->get_arg(1)), m);  // arg0HasArg1 = Contains(args[0], args[1])
    items.push_back(ctx.mk_eq_atom(arg0HasArg1, eq1));


    expr_ref condAst(arg0HasArg1, m);
    //----------------------------
    // true branch
    expr_ref_vector thenItems(m);
    thenItems.push_back(m_autil.mk_ge(indexAst, mk_int(0)));
    //  args[0] = x1 . args[1] . x2
    //  x1 doesn't contain args[1]
    thenItems.push_back(m.mk_not(mk_contains(x2, expr->get_arg(1))));
    thenItems.push_back(ctx.mk_eq_atom(indexAst, mk_strlen(x1)));

    bool canSkip = false;
    if (m_strutil.is_string(expr->get_arg(1))) {
        std::string arg1Str = m_strutil.get_string_constant_value(expr->get_arg(1));
        if (arg1Str.length() == 1) {
            canSkip = true;
        }
    }

    if (!canSkip) {
        // args[0]  = x3 . x4 /\ |x3| = |x1| + 1 /\ ! contains(x4, args[1])
        expr_ref x3(mk_str_var("x3"), m);
        expr_ref x4(mk_str_var("x4"), m);
        expr_ref tmpLen(m_autil.mk_add(indexAst, mk_int(1)), m);
        thenItems.push_back(ctx.mk_eq_atom(expr->get_arg(0), mk_concat(x3, x4)));
        thenItems.push_back(ctx.mk_eq_atom(mk_strlen(x3), tmpLen));
        thenItems.push_back(m.mk_not(mk_contains(x4, expr->get_arg(1))));
    }
    //----------------------------
    // else branch
    expr_ref_vector elseItems(m);
    elseItems.push_back(ctx.mk_eq_atom(indexAst, mk_int(-1)));

    items.push_back(m.mk_ite(condAst, m.mk_and(thenItems.size(), thenItems.c_ptr()), m.mk_and(elseItems.size(), elseItems.c_ptr())));

    expr_ref breakdownAssert(m.mk_and(items.size(), items.c_ptr()), m);
    SASSERT(breakdownAssert);

    expr_ref reduceToIndex(ctx.mk_eq_atom(expr, indexAst), m);
    SASSERT(reduceToIndex);

    expr_ref finalAxiom(m.mk_and(breakdownAssert, reduceToIndex), m);
    SASSERT(finalAxiom);
    assert_axiom(finalAxiom);
}

void theory_str::instantiate_axiom_Substr(enode * e) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    app * expr = e->get_owner();
    if (axiomatized_terms.contains(expr)) {
        TRACE("t_str_detail", tout << "already set up Substr axiom for " << mk_pp(expr, m) << std::endl;);
        return;
    }
    axiomatized_terms.insert(expr);

    TRACE("t_str_detail", tout << "instantiate Substr axiom for " << mk_pp(expr, m) << std::endl;);

    expr_ref ts0(mk_str_var("ts0"), m);
    expr_ref ts1(mk_str_var("ts1"), m);
    expr_ref ts2(mk_str_var("ts2"), m);

    expr_ref ts0_contains_ts1(mk_contains(expr->get_arg(0), ts1), m);

    expr_ref_vector and_item(m);
    // TODO simulate this contains check; it causes problems with a few regressions but we might need it for performance
    //and_item.push_back(ts0_contains_ts1);
    and_item.push_back(ctx.mk_eq_atom(expr->get_arg(0), mk_concat(ts0, mk_concat(ts1, ts2))));
    and_item.push_back(ctx.mk_eq_atom(expr->get_arg(1), mk_strlen(ts0)));
    and_item.push_back(ctx.mk_eq_atom(expr->get_arg(2), mk_strlen(ts1)));

    expr_ref breakdownAssert(m.mk_and(and_item.size(), and_item.c_ptr()), m);
    SASSERT(breakdownAssert);

    expr_ref reduceToVar(ctx.mk_eq_atom(expr, ts1), m);
    SASSERT(reduceToVar);

    expr_ref finalAxiom(m.mk_and(breakdownAssert, reduceToVar), m);
    SASSERT(finalAxiom);
    assert_axiom(finalAxiom);
}

void theory_str::instantiate_axiom_Replace(enode * e) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    app * expr = e->get_owner();
    if (axiomatized_terms.contains(expr)) {
        TRACE("t_str_detail", tout << "already set up Replace axiom for " << mk_pp(expr, m) << std::endl;);
        return;
    }
    axiomatized_terms.insert(expr);

    TRACE("t_str_detail", tout << "instantiate Replace axiom for " << mk_pp(expr, m) << std::endl;);

    expr_ref x1(mk_str_var("x1"), m);
    expr_ref x2(mk_str_var("x2"), m);
    expr_ref i1(mk_int_var("i1"), m);
    expr_ref result(mk_str_var("result"), m);

    // condAst = Contains(args[0], args[1])
    expr_ref condAst(mk_contains(expr->get_arg(0), expr->get_arg(1)), m);
    // -----------------------
    // true branch
    expr_ref_vector thenItems(m);
    //  args[0] = x1 . args[1] . x2
    thenItems.push_back(ctx.mk_eq_atom(expr->get_arg(0), mk_concat(x1, mk_concat(expr->get_arg(1), x2))));
    //  i1 = |x1|
    thenItems.push_back(ctx.mk_eq_atom(i1, mk_strlen(x1)));
    //  args[0]  = x3 . x4 /\ |x3| = |x1| + |args[1]| - 1 /\ ! contains(x3, args[1])
    expr_ref x3(mk_str_var("x3"), m);
    expr_ref x4(mk_str_var("x4"), m);
    expr_ref tmpLen(m_autil.mk_add(i1, mk_strlen(expr->get_arg(1)), mk_int(-1)), m);
    thenItems.push_back(ctx.mk_eq_atom(expr->get_arg(0), mk_concat(x3, x4)));
    thenItems.push_back(ctx.mk_eq_atom(mk_strlen(x3), tmpLen));
    thenItems.push_back(m.mk_not(mk_contains(x3, expr->get_arg(1))));
    thenItems.push_back(ctx.mk_eq_atom(result, mk_concat(x1, mk_concat(expr->get_arg(2), x2))));
    // -----------------------
    // false branch
    expr_ref elseBranch(ctx.mk_eq_atom(result, expr->get_arg(0)), m);

    expr_ref breakdownAssert(m.mk_ite(condAst, m.mk_and(thenItems.size(), thenItems.c_ptr()), elseBranch), m);
    SASSERT(breakdownAssert);

    expr_ref reduceToResult(ctx.mk_eq_atom(expr, result), m);
    SASSERT(reduceToResult);

    expr_ref finalAxiom(m.mk_and(breakdownAssert, reduceToResult), m);
    SASSERT(finalAxiom);
    assert_axiom(finalAxiom);
}

expr * theory_str::mk_RegexIn(expr * str, expr * regexp) {
    expr * args[2] = {str, regexp};
    app * regexIn = get_manager().mk_app(get_id(), OP_RE_REGEXIN, 0, 0, 2, args);
    // immediately force internalization so that axiom setup does not fail
    get_context().internalize(regexIn, false);
    set_up_axioms(regexIn);
    return regexIn;
}

void theory_str::instantiate_axiom_RegexIn(enode * e) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    app * ex = e->get_owner();
    if (axiomatized_terms.contains(ex)) {
        TRACE("t_str_detail", tout << "already set up RegexIn axiom for " << mk_pp(ex, m) << std::endl;);
        return;
    }
    axiomatized_terms.insert(ex);

    TRACE("t_str_detail", tout << "instantiate RegexIn axiom for " << mk_pp(ex, m) << std::endl;);

    {
        std::string regexStr = m_strutil.get_std_regex_str(ex->get_arg(1));
        std::pair<expr*, std::string> key1(ex->get_arg(0), regexStr);
        // skip Z3str's map check, because we already check if we set up axioms on this term
        regex_in_bool_map[key1] = ex;
        regex_in_var_reg_str_map[ex->get_arg(0)].insert(regexStr);
    }

    expr_ref str(ex->get_arg(0), m);
    app * regex = to_app(ex->get_arg(1));

    if (is_Str2Reg(regex)) {
    	expr_ref rxStr(regex->get_arg(0), m);
    	// want to assert 'expr IFF (str == rxStr)'
    	expr_ref rhs(ctx.mk_eq_atom(str, rxStr), m);
    	expr_ref finalAxiom(m.mk_iff(ex, rhs), m);
    	SASSERT(finalAxiom);
    	assert_axiom(finalAxiom);
    } else if (is_RegexConcat(regex)) {
    	expr_ref var1(mk_regex_rep_var(), m);
    	expr_ref var2(mk_regex_rep_var(), m);
    	expr_ref rhs(mk_concat(var1, var2), m);
    	expr_ref rx1(regex->get_arg(0), m);
    	expr_ref rx2(regex->get_arg(1), m);
    	expr_ref var1InRegex1(mk_RegexIn(var1, rx1), m);
    	expr_ref var2InRegex2(mk_RegexIn(var2, rx2), m);

    	expr_ref_vector items(m);
    	items.push_back(var1InRegex1);
    	items.push_back(var2InRegex2);
    	items.push_back(ctx.mk_eq_atom(ex, ctx.mk_eq_atom(str, rhs)));

    	expr_ref finalAxiom(mk_and(items), m);
    	SASSERT(finalAxiom);
    	assert_axiom(finalAxiom);
    } else if (is_RegexUnion(regex)) {
    	expr_ref var1(mk_regex_rep_var(), m);
    	expr_ref var2(mk_regex_rep_var(), m);
    	expr_ref orVar(m.mk_or(ctx.mk_eq_atom(str, var1), ctx.mk_eq_atom(str, var2)), m);
    	expr_ref regex1(regex->get_arg(0), m);
    	expr_ref regex2(regex->get_arg(1), m);
    	expr_ref var1InRegex1(mk_RegexIn(var1, regex1), m);
    	expr_ref var2InRegex2(mk_RegexIn(var2, regex2), m);
    	expr_ref_vector items(m);
    	items.push_back(var1InRegex1);
    	items.push_back(var2InRegex2);
    	items.push_back(ctx.mk_eq_atom(ex, orVar));
    	assert_axiom(mk_and(items));
    } else if (is_RegexStar(regex)) {
    	// slightly more complex due to the unrolling step.
    	expr_ref regex1(regex->get_arg(0), m);
    	expr_ref unrollCount(mk_unroll_bound_var(), m);
    	expr_ref unrollFunc(mk_unroll(regex1, unrollCount), m);
    	expr_ref_vector items(m);
    	items.push_back(ctx.mk_eq_atom(ex, ctx.mk_eq_atom(str, unrollFunc)));
    	items.push_back(ctx.mk_eq_atom(ctx.mk_eq_atom(unrollCount, mk_int(0)), ctx.mk_eq_atom(unrollFunc, m_strutil.mk_string(""))));
    	expr_ref finalAxiom(mk_and(items), m);
    	SASSERT(finalAxiom);
    	assert_axiom(finalAxiom);
    } else {
    	TRACE("t_str_detail", tout << "ERROR: unknown regex expression " << mk_pp(regex, m) << "!" << std::endl;);
    	NOT_IMPLEMENTED_YET();
    }
}

void theory_str::attach_new_th_var(enode * n) {
    context & ctx = get_context();
    theory_var v = mk_var(n);
    ctx.attach_th_var(n, this, v);
    TRACE("t_str_detail", tout << "new theory var: " << mk_ismt2_pp(n->get_owner(), get_manager()) << " := v#" << v << std::endl;);
}

void theory_str::reset_eh() {
    TRACE("t_str", tout << "resetting" << std::endl;);
    m_trail_stack.reset();

    m_basicstr_axiom_todo.reset();
    m_str_eq_todo.reset();
    m_concat_axiom_todo.reset();
    // TODO reset a loooooot more internal stuff
    pop_scope_eh(get_context().get_scope_level());
}

/*
 * Check equality among equivalence class members of LHS and RHS
 * to discover an incorrect LHS == RHS.
 * For example, if we have y2 == "str3"
 * and the equivalence classes are
 * { y2, (Concat ce m2) }
 * { "str3", (Concat abc x2) }
 * then y2 can't be equal to "str3".
 * Then add an assertion: (y2 == (Concat ce m2)) AND ("str3" == (Concat abc x2)) -> (y2 != "str3")
 */
bool theory_str::new_eq_check(expr * lhs, expr * rhs) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    // skip this check if we defer consistency checking, as we can do it for every EQC in final check
    if (!opt_DeferEQCConsistencyCheck) {
        check_concat_len_in_eqc(lhs);
        check_concat_len_in_eqc(rhs);
    }

    // Now we iterate over all pairs of terms across both EQCs
    // and check whether we can show that any pair of distinct terms
    // cannot possibly be equal.
    // If that's the case, we assert an axiom to that effect and stop.

    expr * eqc_nn1 = lhs;
    do {
        expr * eqc_nn2 = rhs;
        do {
            TRACE("t_str_detail", tout << "checking whether " << mk_pp(eqc_nn1, m) << " and " << mk_pp(eqc_nn2, m) << " can be equal" << std::endl;);
            // inconsistency check: value
            if (!can_two_nodes_eq(eqc_nn1, eqc_nn2)) {
                TRACE("t_str", tout << "inconsistency detected: " << mk_pp(eqc_nn1, m) << " cannot be equal to " << mk_pp(eqc_nn2, m) << std::endl;);
                expr_ref to_assert(m.mk_not(ctx.mk_eq_atom(eqc_nn1, eqc_nn2)), m);
                assert_axiom(to_assert);
                // this shouldn't use the integer theory at all, so we don't allow the option of quick-return
                return false;
            }
            if (!check_length_consistency(eqc_nn1, eqc_nn2)) {
                TRACE("t_str", tout << "inconsistency detected: " << mk_pp(eqc_nn1, m) << " and " << mk_pp(eqc_nn2, m) << " have inconsistent lengths" << std::endl;);
                if (opt_NoQuickReturn_IntegerTheory){
                    TRACE("t_str_detail", tout << "continuing in new_eq_check() due to opt_NoQuickReturn_IntegerTheory" << std::endl;);
                } else {
                    return false;
                }
            }
            eqc_nn2 = get_eqc_next(eqc_nn2);
        } while (eqc_nn2 != rhs);
        eqc_nn1 = get_eqc_next(eqc_nn1);
    } while (eqc_nn1 != lhs);

    if (!contains_map.empty()) {
        check_contain_in_new_eq(lhs, rhs);
    }

    if (!regex_in_bool_map.empty()) {
        TRACE("t_str", tout << "checking regex consistency" << std::endl;);
        check_regex_in(lhs, rhs);
    }

    // okay, all checks here passed
    return true;
}

// support for user_smt_theory-style EQC handling

app * theory_str::get_ast(theory_var i) {
    return get_enode(i)->get_owner();
}

theory_var theory_str::get_var(expr * n) const {
    if (!is_app(n)) {
        return null_theory_var;
    }
    context & ctx = get_context();
    if (ctx.e_internalized(to_app(n))) {
        enode * e = ctx.get_enode(to_app(n));
        return e->get_th_var(get_id());
    }
    return null_theory_var;
}

// simulate Z3_theory_get_eqc_next()
expr * theory_str::get_eqc_next(expr * n) {
    theory_var v = get_var(n);
    if (v != null_theory_var) {
        theory_var r = m_find.next(v);
        return get_ast(r);
    }
    return n;
}

void theory_str::group_terms_by_eqc(expr * n, std::set<expr*> & concats, std::set<expr*> & vars, std::set<expr*> & consts) {
    context & ctx = get_context();
    expr * eqcNode = n;
    do {
        app * ast = to_app(eqcNode);
        if (is_concat(ast)) {
            expr * simConcat = simplify_concat(ast);
            if (simConcat != ast) {
                if (is_concat(to_app(simConcat))) {
                    concats.insert(simConcat);
                } else {
                    if (m_strutil.is_string(simConcat)) {
                        consts.insert(simConcat);
                    } else {
                        vars.insert(simConcat);
                    }
                }
            } else {
                concats.insert(simConcat);
            }
        } else if (is_string(ast)) {
            consts.insert(ast);
        } else {
            vars.insert(ast);
        }
        eqcNode = get_eqc_next(eqcNode);
    } while (eqcNode != n);
}

void theory_str::get_nodes_in_concat(expr * node, ptr_vector<expr> & nodeList) {
    app * a_node = to_app(node);
    if (!is_concat(a_node)) {
        nodeList.push_back(node);
        return;
    } else {
        SASSERT(a_node->get_num_args() == 2);
        expr * leftArg = a_node->get_arg(0);
        expr * rightArg = a_node->get_arg(1);
        get_nodes_in_concat(leftArg, nodeList);
        get_nodes_in_concat(rightArg, nodeList);
    }
}

// previously Concat() in strTheory.cpp
// Evaluates the concatenation (n1 . n2) with respect to
// the current equivalence classes of n1 and n2.
// Returns a constant string expression representing this concatenation
// if one can be determined, or NULL if this is not possible.
expr * theory_str::eval_concat(expr * n1, expr * n2) {
    bool n1HasEqcValue = false;
    bool n2HasEqcValue = false;
    expr * v1 = get_eqc_value(n1, n1HasEqcValue);
    expr * v2 = get_eqc_value(n2, n2HasEqcValue);
    if (n1HasEqcValue && n2HasEqcValue) {
        std::string n1_str = m_strutil.get_string_constant_value(v1);
        std::string n2_str = m_strutil.get_string_constant_value(v2);
        std::string result = n1_str + n2_str;
        return m_strutil.mk_string(result);
    } else if (n1HasEqcValue && !n2HasEqcValue) {
        if (m_strutil.get_string_constant_value(v1) == "") {
            return n2;
        }
    } else if (n2HasEqcValue && !n1HasEqcValue) {
        if (m_strutil.get_string_constant_value(v2) == "") {
            return n1;
        }
    }
    // give up
    return NULL;
}

static inline std::string rational_to_string_if_exists(const rational & x, bool x_exists) {
    if (x_exists) {
        return x.to_string();
    } else {
        return "?";
    }
}

/*
 * The inputs:
 *    ~ nn: non const node
 *    ~ eq_str: the equivalent constant string of nn
 *  Iterate the parent of all eqc nodes of nn, looking for:
 *    ~ concat node
 *  to see whether some concat nodes can be simplified.
 */
void theory_str::simplify_parent(expr * nn, expr * eq_str) {
    ast_manager & m = get_manager();
    context & ctx = get_context();

    TRACE("t_str", tout << "simplifying parents of " << mk_ismt2_pp(nn, m)
            << " with respect to " << mk_ismt2_pp(eq_str, m) << std::endl;);

    ctx.internalize(nn, false);

    std::string eq_strValue = m_strutil.get_string_constant_value(eq_str);
    expr * n_eqNode = nn;
    do {
        enode * n_eq_enode = ctx.get_enode(n_eqNode);
        TRACE("t_str_detail", tout << "considering all parents of " << mk_ismt2_pp(n_eqNode, m) << std::endl
                << "associated n_eq_enode has " << n_eq_enode->get_num_parents() << " parents" << std::endl;);

        // the goal of this next bit is to avoid dereferencing a bogus e_parent in the following loop.
        // what I imagine is causing this bug is that, for example, we examine some parent, we add an axiom that involves it,
        // and the parent_it iterator becomes invalidated, because we indirectly modified the container that we're iterating over.

        enode_vector current_parents;
        for (enode_vector::const_iterator parent_it = n_eq_enode->begin_parents(); parent_it != n_eq_enode->end_parents(); parent_it++) {
            current_parents.insert(*parent_it);
        }

        for (enode_vector::iterator parent_it = current_parents.begin(); parent_it != current_parents.end(); ++parent_it) {
            enode * e_parent = *parent_it;
            SASSERT(e_parent != NULL);

            app * a_parent = e_parent->get_owner();
            TRACE("t_str_detail", tout << "considering parent " << mk_ismt2_pp(a_parent, m) << std::endl;);

            if (is_concat(a_parent)) {
                expr * arg0 = a_parent->get_arg(0);
                expr * arg1 = a_parent->get_arg(1);

                rational parentLen;
                bool parentLen_exists = get_len_value(a_parent, parentLen);

                if (arg0 == n_eq_enode->get_owner()) {
                    rational arg0Len, arg1Len;
                    bool arg0Len_exists = get_len_value(eq_str, arg0Len);
                    bool arg1Len_exists = get_len_value(arg1, arg1Len);

                    TRACE("t_str_detail",
                            tout << "simplify_parent #1:" << std::endl
                            << "* parent = " << mk_ismt2_pp(a_parent, m) << std::endl
                            << "* |parent| = " << rational_to_string_if_exists(parentLen, parentLen_exists) << std::endl
                            << "* |arg0| = " << rational_to_string_if_exists(arg0Len, arg0Len_exists) << std::endl
                            << "* |arg1| = " << rational_to_string_if_exists(arg1Len, arg1Len_exists) << std::endl;
                    );

                    if (parentLen_exists && !arg1Len_exists) {
                        TRACE("t_str_detail", tout << "make up len for arg1" << std::endl;);
                        expr_ref implyL11(m.mk_and(ctx.mk_eq_atom(mk_strlen(a_parent), mk_int(parentLen)),
                                ctx.mk_eq_atom(mk_strlen(arg0), mk_int(arg0Len))), m);
                        rational makeUpLenArg1 = parentLen - arg0Len;
                        if (makeUpLenArg1.is_nonneg()) {
                            expr_ref implyR11(ctx.mk_eq_atom(mk_strlen(arg1), mk_int(makeUpLenArg1)), m);
                            assert_implication(implyL11, implyR11);
                        } else {
                            expr_ref neg(m.mk_not(implyL11), m);
                            assert_axiom(neg);
                        }
                    }

                    // (Concat n_eqNode arg1) /\ arg1 has eq const

                    expr * concatResult = eval_concat(eq_str, arg1);
                    if (concatResult != NULL) {
                        bool arg1HasEqcValue = false;
                        expr * arg1Value = get_eqc_value(arg1, arg1HasEqcValue);
                        expr_ref implyL(m);
                        if (arg1 != arg1Value) {
                            expr_ref eq_ast1(m);
                            eq_ast1 = ctx.mk_eq_atom(n_eqNode, eq_str);
                            SASSERT(eq_ast1);

                            expr_ref eq_ast2(m);
                            eq_ast2 = ctx.mk_eq_atom(arg1, arg1Value);
                            SASSERT(eq_ast2);

                            implyL = m.mk_and(eq_ast1, eq_ast2);
                        } else {
                            implyL = ctx.mk_eq_atom(n_eqNode, eq_str);
                        }


                        if (!in_same_eqc(a_parent, concatResult)) {
                            expr_ref implyR(m);
                            implyR = ctx.mk_eq_atom(a_parent, concatResult);
                            SASSERT(implyR);

                            assert_implication(implyL, implyR);
                        }
                    } else if (is_concat(to_app(n_eqNode))) {
                        expr_ref simpleConcat(m);
                        simpleConcat = mk_concat(eq_str, arg1);
                        if (!in_same_eqc(a_parent, simpleConcat)) {
                            expr_ref implyL(m);
                            implyL = ctx.mk_eq_atom(n_eqNode, eq_str);
                            SASSERT(implyL);

                            expr_ref implyR(m);
                            implyR = ctx.mk_eq_atom(a_parent, simpleConcat);
                            SASSERT(implyR);
                            assert_implication(implyL, implyR);
                        }
                    }
                } // if (arg0 == n_eq_enode->get_owner())

                if (arg1 == n_eq_enode->get_owner()) {
                    rational arg0Len, arg1Len;
                    bool arg0Len_exists = get_len_value(arg0, arg0Len);
                    bool arg1Len_exists = get_len_value(eq_str, arg1Len);

                    TRACE("t_str_detail",
                            tout << "simplify_parent #2:" << std::endl
                            << "* parent = " << mk_ismt2_pp(a_parent, m) << std::endl
                            << "* |parent| = " << rational_to_string_if_exists(parentLen, parentLen_exists) << std::endl
                            << "* |arg0| = " << rational_to_string_if_exists(arg0Len, arg0Len_exists) << std::endl
                            << "* |arg1| = " << rational_to_string_if_exists(arg1Len, arg1Len_exists) << std::endl;
                    );

                    if (parentLen_exists && !arg0Len_exists) {
                        TRACE("t_str_detail", tout << "make up len for arg0" << std::endl;);
                        expr_ref implyL11(m.mk_and(ctx.mk_eq_atom(mk_strlen(a_parent), mk_int(parentLen)),
                                ctx.mk_eq_atom(mk_strlen(arg1), mk_int(arg1Len))), m);
                        rational makeUpLenArg0 = parentLen - arg1Len;
                        if (makeUpLenArg0.is_nonneg()) {
                            expr_ref implyR11(ctx.mk_eq_atom(mk_strlen(arg0), mk_int(makeUpLenArg0)), m);
                            assert_implication(implyL11, implyR11);
                        } else {
                            expr_ref neg(m.mk_not(implyL11), m);
                            assert_axiom(neg);
                        }
                    }

                    // (Concat arg0 n_eqNode) /\ arg0 has eq const

                    expr * concatResult = eval_concat(arg0, eq_str);
                    if (concatResult != NULL) {
                        bool arg0HasEqcValue = false;
                        expr * arg0Value = get_eqc_value(arg0, arg0HasEqcValue);
                        expr_ref implyL(m);
                        if (arg0 != arg0Value) {
                            expr_ref eq_ast1(m);
                            eq_ast1 = ctx.mk_eq_atom(n_eqNode, eq_str);
                            SASSERT(eq_ast1);

                            expr_ref eq_ast2(m);
                            eq_ast2 = ctx.mk_eq_atom(arg0, arg0Value);
                            SASSERT(eq_ast2);

                            implyL = m.mk_and(eq_ast1, eq_ast2);
                        } else {
                            implyL = ctx.mk_eq_atom(n_eqNode, eq_str);
                        }

                        if (!in_same_eqc(a_parent, concatResult)) {
                            expr_ref implyR(m);
                            implyR = ctx.mk_eq_atom(a_parent, concatResult);
                            SASSERT(implyR);

                            assert_implication(implyL, implyR);
                        }
                    } else if (is_concat(to_app(n_eqNode))) {
                        expr_ref simpleConcat(m);
                        simpleConcat = mk_concat(arg0, eq_str);
                        if (!in_same_eqc(a_parent, simpleConcat)) {
                            expr_ref implyL(m);
                            implyL = ctx.mk_eq_atom(n_eqNode, eq_str);
                            SASSERT(implyL);

                            expr_ref implyR(m);
                            implyR = ctx.mk_eq_atom(a_parent, simpleConcat);
                            SASSERT(implyR);
                            assert_implication(implyL, implyR);
                        }
                    }
                } // if (arg1 == n_eq_enode->get_owner


                //---------------------------------------------------------
                // Case (2-1) begin: (Concat n_eqNode (Concat str var))
                if (arg0 == n_eqNode && is_concat(to_app(arg1))) {
                    app * a_arg1 = to_app(arg1);
                    TRACE("t_str_detail", tout << "simplify_parent #3" << std::endl;);
                    expr * r_concat_arg0 = a_arg1->get_arg(0);
                    if (m_strutil.is_string(r_concat_arg0)) {
                        expr * combined_str = eval_concat(eq_str, r_concat_arg0);
                        SASSERT(combined_str);
                        expr * r_concat_arg1 = a_arg1->get_arg(1);
                        expr_ref implyL(m);
                        implyL = ctx.mk_eq_atom(n_eqNode, eq_str);
                        expr * simplifiedAst = mk_concat(combined_str, r_concat_arg1);
                        if (!in_same_eqc(a_parent, simplifiedAst)) {
                            expr_ref implyR(m);
                            implyR = ctx.mk_eq_atom(a_parent, simplifiedAst);
                            assert_implication(implyL, implyR);
                        }
                    }
                }
                // Case (2-1) end: (Concat n_eqNode (Concat str var))
                //---------------------------------------------------------


                //---------------------------------------------------------
                // Case (2-2) begin: (Concat (Concat var str) n_eqNode)
                if (is_concat(to_app(arg0)) && arg1 == n_eqNode) {
                    app * a_arg0 = to_app(arg0);
                    TRACE("t_str_detail", tout << "simplify_parent #4" << std::endl;);
                    expr * l_concat_arg1 = a_arg0->get_arg(1);
                    if (m_strutil.is_string(l_concat_arg1)) {
                        expr * combined_str = eval_concat(l_concat_arg1, eq_str);
                        SASSERT(combined_str);
                        expr * l_concat_arg0 = a_arg0->get_arg(0);
                        expr_ref implyL(m);
                        implyL = ctx.mk_eq_atom(n_eqNode, eq_str);
                        expr * simplifiedAst = mk_concat(l_concat_arg0, combined_str);
                        if (!in_same_eqc(a_parent, simplifiedAst)) {
                            expr_ref implyR(m);
                            implyR = ctx.mk_eq_atom(a_parent, simplifiedAst);
                            assert_implication(implyL, implyR);
                        }
                    }
                }
                // Case (2-2) end: (Concat (Concat var str) n_eqNode)
                //---------------------------------------------------------

                // Have to look up one more layer: if the parent of the concat is another concat
                //-------------------------------------------------
                // Case (3-1) begin: (Concat (Concat var n_eqNode) str )
                if (arg1 == n_eqNode) {
                    for (enode_vector::iterator concat_parent_it = e_parent->begin_parents();
                            concat_parent_it != e_parent->end_parents(); concat_parent_it++) {
                        enode * e_concat_parent = *concat_parent_it;
                        app * concat_parent = e_concat_parent->get_owner();
                        if (is_concat(concat_parent)) {
                            expr * concat_parent_arg0 = concat_parent->get_arg(0);
                            expr * concat_parent_arg1 = concat_parent->get_arg(1);
                            if (concat_parent_arg0 == a_parent && m_strutil.is_string(concat_parent_arg1)) {
                                TRACE("t_str_detail", tout << "simplify_parent #5" << std::endl;);
                                expr * combinedStr = eval_concat(eq_str, concat_parent_arg1);
                                SASSERT(combinedStr);
                                expr_ref implyL(m);
                                implyL = ctx.mk_eq_atom(n_eqNode, eq_str);
                                expr * simplifiedAst = mk_concat(arg0, combinedStr);
                                if (!in_same_eqc(concat_parent, simplifiedAst)) {
                                    expr_ref implyR(m);
                                    implyR = ctx.mk_eq_atom(concat_parent, simplifiedAst);
                                    assert_implication(implyL, implyR);
                                }
                            }
                        }
                    }
                }
                // Case (3-1) end: (Concat (Concat var n_eqNode) str )
                // Case (3-2) begin: (Concat str (Concat n_eqNode var) )
                if (arg0 == n_eqNode) {
                    for (enode_vector::iterator concat_parent_it = e_parent->begin_parents();
                            concat_parent_it != e_parent->end_parents(); concat_parent_it++) {
                        enode * e_concat_parent = *concat_parent_it;
                        app * concat_parent = e_concat_parent->get_owner();
                        if (is_concat(concat_parent)) {
                            expr * concat_parent_arg0 = concat_parent->get_arg(0);
                            expr * concat_parent_arg1 = concat_parent->get_arg(1);
                            if (concat_parent_arg1 == a_parent && m_strutil.is_string(concat_parent_arg0)) {
                                TRACE("t_str_detail", tout << "simplify_parent #6" << std::endl;);
                                expr * combinedStr = eval_concat(concat_parent_arg0, eq_str);
                                SASSERT(combinedStr);
                                expr_ref implyL(m);
                                implyL = ctx.mk_eq_atom(n_eqNode, eq_str);
                                expr * simplifiedAst = mk_concat(combinedStr, arg1);
                                if (!in_same_eqc(concat_parent, simplifiedAst)) {
                                    expr_ref implyR(m);
                                    implyR = ctx.mk_eq_atom(concat_parent, simplifiedAst);
                                    assert_implication(implyL, implyR);
                                }
                            }
                        }
                    }
                }
                // Case (3-2) end: (Concat str (Concat n_eqNode var) )
            } // if is_concat(a_parent)
        } // for parent_it : n_eq_enode->begin_parents()


        // check next EQC member
        n_eqNode = get_eqc_next(n_eqNode);
    } while (n_eqNode != nn);
}

expr * theory_str::simplify_concat(expr * node) {
    ast_manager & m = get_manager();
    context & ctx = get_context();
    std::map<expr*, expr*> resolvedMap;
    ptr_vector<expr> argVec;
    get_nodes_in_concat(node, argVec);

    for (unsigned i = 0; i < argVec.size(); ++i) {
        bool vArgHasEqcValue = false;
        expr * vArg = get_eqc_value(argVec[i], vArgHasEqcValue);
        if (vArg != argVec[i]) {
            resolvedMap[argVec[i]] = vArg;
        }
    }

    if (resolvedMap.size() == 0) {
        // no simplification possible
        return node;
    } else {
        expr * resultAst = m_strutil.mk_string("");
        for (unsigned i = 0; i < argVec.size(); ++i) {
            bool vArgHasEqcValue = false;
            expr * vArg = get_eqc_value(argVec[i], vArgHasEqcValue);
            resultAst = mk_concat(resultAst, vArg);
        }
        TRACE("t_str_detail", tout << mk_ismt2_pp(node, m) << " is simplified to " << mk_ismt2_pp(resultAst, m) << std::endl;);

        if (in_same_eqc(node, resultAst)) {
            TRACE("t_str_detail", tout << "SKIP: both concats are already in the same equivalence class" << std::endl;);
        } else {
            // TODO refactor
            expr ** items = alloc_svect(expr*, resolvedMap.size());
            int pos = 0;
            std::map<expr*, expr*>::iterator itor = resolvedMap.begin();
            for (; itor != resolvedMap.end(); ++itor) {
                items[pos++] = ctx.mk_eq_atom(itor->first, itor->second);
            }
            expr_ref premise(m);
            if (pos == 1) {
                premise = items[0];
            } else {
                premise = m.mk_and(pos, items);
            }
            expr_ref conclusion(ctx.mk_eq_atom(node, resultAst), m);
            assert_implication(premise, conclusion);
        }
        return resultAst;
    }

}

// Modified signature of Z3str2's inferLenConcat().
// Returns true iff nLen can be inferred by this method
// (i.e. the equivalent of a len_exists flag in get_len_value()).

bool theory_str::infer_len_concat(expr * n, rational & nLen) {
	context & ctx = get_context();
	ast_manager & m = get_manager();
	expr * arg0 = to_app(n)->get_arg(0);
	expr * arg1 = to_app(n)->get_arg(1);

	rational arg0_len, arg1_len;
	bool arg0_len_exists = get_len_value(arg0, arg0_len);
	bool arg1_len_exists = get_len_value(arg1, arg1_len);
	rational tmp_len;
	bool nLen_exists = get_len_value(n, tmp_len);

	if (arg0_len_exists && arg1_len_exists && !nLen_exists) {
		expr_ref_vector l_items(m);
		// if (mk_strlen(arg0) != mk_int(arg0_len)) {
		{
			l_items.push_back(ctx.mk_eq_atom(mk_strlen(arg0), mk_int(arg0_len)));
		}

		// if (mk_strlen(arg1) != mk_int(arg1_len)) {
		{
			l_items.push_back(ctx.mk_eq_atom(mk_strlen(arg1), mk_int(arg1_len)));
		}

		expr_ref axl(m.mk_and(l_items.size(), l_items.c_ptr()), m);
		rational nnLen = arg0_len + arg1_len;
		expr_ref axr(ctx.mk_eq_atom(mk_strlen(n), mk_int(nnLen)), m);
		TRACE("t_str_detail", tout << "inferred (Length " << mk_pp(n, m) << ") = " << nnLen << std::endl;);
		assert_implication(axl, axr);
		nLen = nnLen;
		return true;
	} else {
		return false;
	}
}

void theory_str::infer_len_concat_arg(expr * n, rational len) {
	if (len.is_neg()) {
		return;
	}

	context & ctx = get_context();
	ast_manager & m = get_manager();

	expr * arg0 = to_app(n)->get_arg(0);
	expr * arg1 = to_app(n)->get_arg(1);
	rational arg0_len, arg1_len;
	bool arg0_len_exists = get_len_value(arg0, arg0_len);
	bool arg1_len_exists = get_len_value(arg1, arg1_len);

	expr_ref_vector l_items(m);
	expr_ref axr(m);
	axr.reset();

	// if (mk_length(t, n) != mk_int(ctx, len)) {
	{
		l_items.push_back(ctx.mk_eq_atom(mk_strlen(n), mk_int(len)));
	}

	if (!arg0_len_exists && arg1_len_exists) {
		//if (mk_length(t, arg1) != mk_int(ctx, arg1_len)) {
		{
			l_items.push_back(ctx.mk_eq_atom(mk_strlen(arg1), mk_int(arg1_len)));
		}
		rational arg0Len = len - arg1_len;
		if (arg0Len.is_nonneg()) {
			axr = ctx.mk_eq_atom(mk_strlen(arg0), mk_int(arg0Len));
		} else {
			// TODO negate?
		}
	} else if (arg0_len_exists && !arg1_len_exists) {
		//if (mk_length(t, arg0) != mk_int(ctx, arg0_len)) {
		{
			l_items.push_back(ctx.mk_eq_atom(mk_strlen(arg0), mk_int(arg0_len)));
		}
		rational arg1Len = len - arg0_len;
		if (arg1Len.is_nonneg()) {
			axr = ctx.mk_eq_atom(mk_strlen(arg1), mk_int(arg1Len));
		} else {
			// TODO negate?
		}
	} else {

	}

	if (axr) {
		expr_ref axl(m.mk_and(l_items.size(), l_items.c_ptr()), m);
		assert_implication(axl, axr);
	}
}

void theory_str::infer_len_concat_equality(expr * nn1, expr * nn2) {
    rational nnLen;
    bool nnLen_exists = get_len_value(nn1, nnLen);
    if (!nnLen_exists) {
        nnLen_exists = get_len_value(nn2, nnLen);
    }

    // case 1:
    //    Known: a1_arg0 and a1_arg1
    //    Unknown: nn1

    if (is_concat(to_app(nn1))) {
        rational nn1ConcatLen;
        bool nn1ConcatLen_exists = infer_len_concat(nn1, nn1ConcatLen);
        if (nnLen_exists && nn1ConcatLen_exists) {
            nnLen = nn1ConcatLen;
        }
    }

    // case 2:
    //    Known: a1_arg0 and a1_arg1
    //    Unknown: nn1

    if (is_concat(to_app(nn2))) {
        rational nn2ConcatLen;
        bool nn2ConcatLen_exists = infer_len_concat(nn2, nn2ConcatLen);
        if (nnLen_exists && nn2ConcatLen_exists) {
            nnLen = nn2ConcatLen;
        }
    }

    if (nnLen_exists) {
        if (is_concat(to_app(nn1))) {
            infer_len_concat_arg(nn1, nnLen);
        }
        if (is_concat(to_app(nn2))) {
            infer_len_concat_arg(nn2, nnLen);
        }
    }

    /*
    if (isConcatFunc(t, nn2)) {
        int nn2ConcatLen = inferLenConcat(t, nn2);
        if (nnLen == -1 && nn2ConcatLen != -1)
            nnLen = nn2ConcatLen;
    }

    if (nnLen != -1) {
        if (isConcatFunc(t, nn1)) {
            inferLenConcatArg(t, nn1, nnLen);
        }
        if (isConcatFunc(t, nn2)) {
            inferLenConcatArg(t, nn2, nnLen);
        }
    }
    */
}

/*
 * Handle two equivalent Concats.
 */
void theory_str::simplify_concat_equality(expr * nn1, expr * nn2) {
    ast_manager & m = get_manager();
    context & ctx = get_context();

    app * a_nn1 = to_app(nn1);
    SASSERT(a_nn1->get_num_args() == 2);
    app * a_nn2 = to_app(nn2);
    SASSERT(a_nn2->get_num_args() == 2);

    expr * a1_arg0 = a_nn1->get_arg(0);
    expr * a1_arg1 = a_nn1->get_arg(1);
    expr * a2_arg0 = a_nn2->get_arg(0);
    expr * a2_arg1 = a_nn2->get_arg(1);

    rational a1_arg0_len, a1_arg1_len, a2_arg0_len, a2_arg1_len;

    bool a1_arg0_len_exists = get_len_value(a1_arg0, a1_arg0_len);
    bool a1_arg1_len_exists = get_len_value(a1_arg1, a1_arg1_len);
    bool a2_arg0_len_exists = get_len_value(a2_arg0, a2_arg0_len);
    bool a2_arg1_len_exists = get_len_value(a2_arg1, a2_arg1_len);

    TRACE("t_str", tout << "nn1 = " << mk_ismt2_pp(nn1, m) << std::endl
            << "nn2 = " << mk_ismt2_pp(nn2, m) << std::endl;);

    TRACE("t_str_detail", tout
            << "len(" << mk_pp(a1_arg0, m) << ") = " << (a1_arg0_len_exists ? a1_arg0_len.to_string() : "?") << std::endl
            << "len(" << mk_pp(a1_arg1, m) << ") = " << (a1_arg1_len_exists ? a1_arg1_len.to_string() : "?") << std::endl
            << "len(" << mk_pp(a2_arg0, m) << ") = " << (a2_arg0_len_exists ? a2_arg0_len.to_string() : "?") << std::endl
            << "len(" << mk_pp(a2_arg1, m) << ") = " << (a2_arg1_len_exists ? a2_arg1_len.to_string() : "?") << std::endl
            << std::endl;);

    infer_len_concat_equality(nn1, nn2);

    if (a1_arg0 == a2_arg0) {
        if (!in_same_eqc(a1_arg1, a2_arg1)) {
            expr_ref premise(ctx.mk_eq_atom(nn1, nn2), m);
            expr_ref eq1(ctx.mk_eq_atom(a1_arg1, a2_arg1), m);
            expr_ref eq2(ctx.mk_eq_atom(mk_strlen(a1_arg1), mk_strlen(a2_arg1)), m);
            expr_ref conclusion(m.mk_and(eq1, eq2), m);
            assert_implication(premise, conclusion);
        }
        TRACE("t_str_detail", tout << "SKIP: a1_arg0 == a2_arg0" << std::endl;);
        return;
    }

    if (a1_arg1 == a2_arg1) {
        if (!in_same_eqc(a1_arg0, a2_arg0)) {
            expr_ref premise(ctx.mk_eq_atom(nn1, nn2), m);
            expr_ref eq1(ctx.mk_eq_atom(a1_arg0, a2_arg0), m);
            expr_ref eq2(ctx.mk_eq_atom(mk_strlen(a1_arg0), mk_strlen(a2_arg0)), m);
            expr_ref conclusion(m.mk_and(eq1, eq2), m);
            assert_implication(premise, conclusion);
        }
        TRACE("t_str_detail", tout << "SKIP: a1_arg1 == a2_arg1" << std::endl;);
        return;
    }

    // quick path

    if (in_same_eqc(a1_arg0, a2_arg0)) {
        if (in_same_eqc(a1_arg1, a2_arg1)) {
            TRACE("t_str_detail", tout << "SKIP: a1_arg0 =~ a2_arg0 and a1_arg1 =~ a2_arg1" << std::endl;);
            return;
        } else {
            TRACE("t_str_detail", tout << "quick path 1-1: a1_arg0 =~ a2_arg0" << std::endl;);
            expr_ref premise(m.mk_and(ctx.mk_eq_atom(nn1, nn2), ctx.mk_eq_atom(a1_arg0, a2_arg0)), m);
            expr_ref conclusion(m.mk_and(ctx.mk_eq_atom(a1_arg1, a2_arg1), ctx.mk_eq_atom(mk_strlen(a1_arg1), mk_strlen(a2_arg1))), m);
            assert_implication(premise, conclusion);
            return;
        }
    } else {
        if (in_same_eqc(a1_arg1, a2_arg1)) {
            TRACE("t_str_detail", tout << "quick path 1-2: a1_arg1 =~ a2_arg1" << std::endl;);
            expr_ref premise(m.mk_and(ctx.mk_eq_atom(nn1, nn2), ctx.mk_eq_atom(a1_arg1, a2_arg1)), m);
            expr_ref conclusion(m.mk_and(ctx.mk_eq_atom(a1_arg0, a2_arg0), ctx.mk_eq_atom(mk_strlen(a1_arg0), mk_strlen(a2_arg0))), m);
            assert_implication(premise, conclusion);
            return;
        }
    }

    // quick path 2-1
    if (a1_arg0_len_exists && a2_arg0_len_exists && a1_arg0_len == a2_arg0_len) {
        if (!in_same_eqc(a1_arg0, a2_arg0)) {
            TRACE("t_str_detail", tout << "quick path 2-1: len(nn1.arg0) == len(nn2.arg0)" << std::endl;);
            expr_ref ax_l1(ctx.mk_eq_atom(nn1, nn2), m);
            expr_ref ax_l2(ctx.mk_eq_atom(mk_strlen(a1_arg0), mk_strlen(a2_arg0)), m);
            expr_ref ax_r1(ctx.mk_eq_atom(a1_arg0, a2_arg0), m);
            expr_ref ax_r2(ctx.mk_eq_atom(a1_arg1, a2_arg1), m);

            expr_ref premise(m.mk_and(ax_l1, ax_l2), m);
            expr_ref conclusion(m.mk_and(ax_r1, ax_r2), m);

            assert_implication(premise, conclusion);

            if (opt_NoQuickReturn_IntegerTheory) {
                TRACE("t_str_detail", tout << "bypassing quick return from the end of this case" << std::endl;);
            } else {
                return;
            }
        }
    }

    if (a1_arg1_len_exists && a2_arg1_len_exists && a1_arg1_len == a2_arg1_len) {
        if (!in_same_eqc(a1_arg1, a2_arg1)) {
            TRACE("t_str_detail", tout << "quick path 2-2: len(nn1.arg1) == len(nn2.arg1)" << std::endl;);
            expr_ref ax_l1(ctx.mk_eq_atom(nn1, nn2), m);
            expr_ref ax_l2(ctx.mk_eq_atom(mk_strlen(a1_arg1), mk_strlen(a2_arg1)), m);
            expr_ref ax_r1(ctx.mk_eq_atom(a1_arg0, a2_arg0), m);
            expr_ref ax_r2(ctx.mk_eq_atom(a1_arg1, a2_arg1), m);

            expr_ref premise(m.mk_and(ax_l1, ax_l2), m);
            expr_ref conclusion(m.mk_and(ax_r1, ax_r2), m);

            assert_implication(premise, conclusion);
            if (opt_NoQuickReturn_IntegerTheory) {
                TRACE("t_str_detail", tout << "bypassing quick return from the end of this case" << std::endl;);
            } else {
                return;
            }
        }
    }

    expr_ref new_nn1(simplify_concat(nn1), m);
    expr_ref new_nn2(simplify_concat(nn2), m);
    app * a_new_nn1 = to_app(new_nn1);
    app * a_new_nn2 = to_app(new_nn2);

    TRACE("t_str_detail", tout << "new_nn1 = " << mk_ismt2_pp(new_nn1, m) << std::endl
            << "new_nn2 = " << mk_ismt2_pp(new_nn2, m) << std::endl;);

    if (new_nn1 == new_nn2) {
        TRACE("t_str_detail", tout << "equal concats, return" << std::endl;);
        return;
    }

    if (!can_two_nodes_eq(new_nn1, new_nn2)) {
        expr_ref detected(m.mk_not(ctx.mk_eq_atom(new_nn1, new_nn2)), m);
        TRACE("t_str_detail", tout << "inconsistency detected: " << mk_ismt2_pp(detected, m) << std::endl;);
        assert_axiom(detected);
        return;
    }

    // check whether new_nn1 and new_nn2 are still concats

    bool n1IsConcat = is_concat(a_new_nn1);
    bool n2IsConcat = is_concat(a_new_nn2);
    if (!n1IsConcat && n2IsConcat) {
        TRACE("t_str_detail", tout << "nn1_new is not a concat" << std::endl;);
        if (is_string(a_new_nn1)) {
            simplify_parent(new_nn2, new_nn1);
        }
        return;
    } else if (n1IsConcat && !n2IsConcat) {
        TRACE("t_str_detail", tout << "nn2_new is not a concat" << std::endl;);
        if (is_string(a_new_nn2)) {
            simplify_parent(new_nn1, new_nn2);
        }
        return;
    } else if (!n1IsConcat && !n2IsConcat) {
    	// normally this should never happen, because group_terms_by_eqc() should have pre-simplified
    	// as much as possible. however, we make a defensive check here just in case
    	TRACE("t_str_detail", tout << "WARNING: nn1_new and nn2_new both simplify to non-concat terms" << std::endl;);
    	return;
    }

    expr * v1_arg0 = a_new_nn1->get_arg(0);
    expr * v1_arg1 = a_new_nn1->get_arg(1);
    expr * v2_arg0 = a_new_nn2->get_arg(0);
    expr * v2_arg1 = a_new_nn2->get_arg(1);

    if (!in_same_eqc(new_nn1, new_nn2) && (nn1 != new_nn1 || nn2 != new_nn2)) {
        int ii4 = 0;
        expr* item[3];
        if (nn1 != new_nn1) {
            item[ii4++] = ctx.mk_eq_atom(nn1, new_nn1);
        }
        if (nn2 != new_nn2) {
            item[ii4++] = ctx.mk_eq_atom(nn2, new_nn2);
        }
        item[ii4++] = ctx.mk_eq_atom(nn1, nn2);
        expr_ref premise(m.mk_and(ii4, item), m);
        expr_ref conclusion(ctx.mk_eq_atom(new_nn1, new_nn2), m);
        assert_implication(premise, conclusion);
    }

    // start to split both concats
    check_and_init_cut_var(v1_arg0);
    check_and_init_cut_var(v1_arg1);
    check_and_init_cut_var(v2_arg0);
    check_and_init_cut_var(v2_arg1);

    //*************************************************************
    // case 1: concat(x, y) = concat(m, n)
    //*************************************************************
    if (is_concat_eq_type1(new_nn1, new_nn2)) {
        process_concat_eq_type1(new_nn1, new_nn2);
        return;
    }

    //*************************************************************
    // case 2: concat(x, y) = concat(m, "str")
    //*************************************************************
    if (is_concat_eq_type2(new_nn1, new_nn2)) {
        process_concat_eq_type2(new_nn1, new_nn2);
        return;
    }

    //*************************************************************
    // case 3: concat(x, y) = concat("str", n)
    //*************************************************************
    if (is_concat_eq_type3(new_nn1, new_nn2)) {
        process_concat_eq_type3(new_nn1, new_nn2);
        return;
    }

    //*************************************************************
    //  case 4: concat("str1", y) = concat("str2", n)
    //*************************************************************
    if (is_concat_eq_type4(new_nn1, new_nn2)) {
        process_concat_eq_type4(new_nn1, new_nn2);
        return;
    }

    //*************************************************************
    //  case 5: concat(x, "str1") = concat(m, "str2")
    //*************************************************************
    if (is_concat_eq_type5(new_nn1, new_nn2)) {
        process_concat_eq_type5(new_nn1, new_nn2);
        return;
    }
    //*************************************************************
    //  case 6: concat("str1", y) = concat(m, "str2")
    //*************************************************************
    if (is_concat_eq_type6(new_nn1, new_nn2)) {
        process_concat_eq_type6(new_nn1, new_nn2);
        return;
    }

}

/*************************************************************
 * Type 1: concat(x, y) = concat(m, n)
 *         x, y, m and n all variables
 *************************************************************/
bool theory_str::is_concat_eq_type1(expr * concatAst1, expr * concatAst2) {
    expr * x = to_app(concatAst1)->get_arg(0);
    expr * y = to_app(concatAst1)->get_arg(1);
    expr * m = to_app(concatAst2)->get_arg(0);
    expr * n = to_app(concatAst2)->get_arg(1);

    if (!m_strutil.is_string(x) && !m_strutil.is_string(y) && !m_strutil.is_string(m) && !m_strutil.is_string(n)) {
        return true;
    } else {
        return false;
    }
}

void theory_str::process_concat_eq_type1(expr * concatAst1, expr * concatAst2) {
    ast_manager & mgr = get_manager();
    context & ctx = get_context();
    TRACE("t_str_detail", tout << "process_concat_eq TYPE 1" << std::endl
            << "concatAst1 = " << mk_ismt2_pp(concatAst1, mgr) << std::endl
            << "concatAst2 = " << mk_ismt2_pp(concatAst2, mgr) << std::endl;
    );

    if (!is_concat(to_app(concatAst1))) {
        TRACE("t_str_detail", tout << "concatAst1 is not a concat function" << std::endl;);
        return;
    }
    if (!is_concat(to_app(concatAst2))) {
        TRACE("t_str_detail", tout << "concatAst2 is not a concat function" << std::endl;);
        return;
    }
    expr * x = to_app(concatAst1)->get_arg(0);
    expr * y = to_app(concatAst1)->get_arg(1);
    expr * m = to_app(concatAst2)->get_arg(0);
    expr * n = to_app(concatAst2)->get_arg(1);

    rational x_len, y_len, m_len, n_len;
    bool x_len_exists = get_len_value(x, x_len);
    bool y_len_exists = get_len_value(y, y_len);
    bool m_len_exists = get_len_value(m, m_len);
    bool n_len_exists = get_len_value(n, n_len);

    int splitType = -1;
    if (x_len_exists && m_len_exists) {
        TRACE("t_str_int", tout << "length values found: x/m" << std::endl;);
        if (x_len < m_len) {
            splitType = 0;
        } else if (x_len == m_len) {
            splitType = 1;
        } else {
            splitType = 2;
        }
    }

    if (splitType == -1 && y_len_exists && n_len_exists) {
        TRACE("t_str_int", tout << "length values found: y/n" << std::endl;);
        if (y_len > n_len) {
            splitType = 0;
        } else if (y_len == n_len) {
            splitType = 1;
        } else {
            splitType = 2;
        }
    }

    TRACE("t_str_detail", tout
    		<< "len(x) = " << (x_len_exists ? x_len.to_string() : "?") << std::endl
    		<< "len(y) = " << (y_len_exists ? y_len.to_string() : "?") << std::endl
			<< "len(m) = " << (m_len_exists ? m_len.to_string() : "?") << std::endl
			<< "len(n) = " << (n_len_exists ? n_len.to_string() : "?") << std::endl
    		<< "split type " << splitType << std::endl;
    );

    expr * t1 = NULL;
    expr * t2 = NULL;
    expr * xorFlag = NULL;

    std::pair<expr*, expr*> key1(concatAst1, concatAst2);
    std::pair<expr*, expr*> key2(concatAst2, concatAst1);

    // check the entries in this map to make sure they're still in scope
    // before we use them.

    std::map<std::pair<expr*,expr*>, std::map<int, expr*> >::iterator entry1 = varForBreakConcat.find(key1);
    std::map<std::pair<expr*,expr*>, std::map<int, expr*> >::iterator entry2 = varForBreakConcat.find(key2);

    bool entry1InScope;
    if (entry1 == varForBreakConcat.end()) {
        entry1InScope = false;
    } else {
        if (internal_variable_set.find((entry1->second)[0]) == internal_variable_set.end()
                || internal_variable_set.find((entry1->second)[1]) == internal_variable_set.end()
                /*|| internal_variable_set.find((entry1->second)[2]) == internal_variable_set.end() */) {
            entry1InScope = false;
        } else {
            entry1InScope = true;
        }
    }

    bool entry2InScope;
    if (entry2 == varForBreakConcat.end()) {
        entry2InScope = false;
    } else {
        if (internal_variable_set.find((entry2->second)[0]) == internal_variable_set.end()
                || internal_variable_set.find((entry2->second)[1]) == internal_variable_set.end()
                /* || internal_variable_set.find((entry2->second)[2]) == internal_variable_set.end() */) {
            entry2InScope = false;
        } else {
            entry2InScope = true;
        }
    }

    TRACE("t_str_detail", tout << "entry 1 " << (entry1InScope ? "in scope" : "not in scope") << std::endl
            << "entry 2 " << (entry2InScope ? "in scope" : "not in scope") << std::endl;);

    if (!entry1InScope && !entry2InScope) {
        t1 = mk_nonempty_str_var();
        t2 = mk_nonempty_str_var();
        xorFlag = mk_internal_xor_var();
        check_and_init_cut_var(t1);
        check_and_init_cut_var(t2);
        varForBreakConcat[key1][0] = t1;
        varForBreakConcat[key1][1] = t2;
        varForBreakConcat[key1][2] = xorFlag;
    } else {
        // match found
        if (entry1InScope) {
            t1 = varForBreakConcat[key1][0];
            t2 = varForBreakConcat[key1][1];
            xorFlag = varForBreakConcat[key1][2];
        } else {
            t1 = varForBreakConcat[key2][0];
            t2 = varForBreakConcat[key2][1];
            xorFlag = varForBreakConcat[key2][2];
        }
        // TODO do I need to refresh the xorFlag, which is an integer var, and if so, how?
        refresh_theory_var(t1);
        refresh_theory_var(t2);
    }

    // For split types 0 through 2, we can get away with providing
    // fewer split options since more length information is available.
    if (splitType == 0) {
        //--------------------------------------
        // Type 0: M cuts Y.
        //   len(x) < len(m) || len(y) > len(n)
        //--------------------------------------
        if (!has_self_cut(m, y)) {
            expr ** ax_l_items = alloc_svect(expr*, 3);
            expr ** ax_r_items = alloc_svect(expr*, 3);

            ax_l_items[0] = ctx.mk_eq_atom(concatAst1, concatAst2);

            expr_ref x_t1(mk_concat(x, t1), mgr);
            expr_ref t1_n(mk_concat(t1, n), mgr);

            ax_r_items[0] = ctx.mk_eq_atom(m, x_t1);
            ax_r_items[1] = ctx.mk_eq_atom(y, t1_n);

            if (m_len_exists && x_len_exists) {
                ax_l_items[1] = ctx.mk_eq_atom(mk_strlen(x), mk_int(x_len));
                ax_l_items[2] = ctx.mk_eq_atom(mk_strlen(m), mk_int(m_len));
                rational m_sub_x = m_len - x_len;
                ax_r_items[2] = ctx.mk_eq_atom(mk_strlen(t1), mk_int(m_sub_x));
            } else {
                ax_l_items[1] = ctx.mk_eq_atom(mk_strlen(y), mk_int(y_len));
                ax_l_items[2] = ctx.mk_eq_atom(mk_strlen(n), mk_int(n_len));
                rational y_sub_n = y_len - n_len;
                ax_r_items[2] = ctx.mk_eq_atom(mk_strlen(t1), mk_int(y_sub_n));
            }

            expr_ref ax_l(mgr.mk_and(3, ax_l_items), mgr);
            expr_ref ax_r(mgr.mk_and(3, ax_r_items), mgr);

            // Cut Info
            add_cut_info_merge(t1, sLevel, m);
            add_cut_info_merge(t1, sLevel, y);

            assert_implication(ax_l, ax_r);
        } else {
            loopDetected = true;
            TRACE("t_str", tout << "AVOID LOOP: SKIPPED" << std::endl;);
            // TODO printCutVar(m, y);
        }
    } else if (splitType == 1) {
        // Type 1:
        //   len(x) = len(m) || len(y) = len(n)
        expr_ref ax_l1(ctx.mk_eq_atom(concatAst1, concatAst2), mgr);
        expr_ref ax_l2(mgr.mk_or(ctx.mk_eq_atom(mk_strlen(x), mk_strlen(m)), ctx.mk_eq_atom(mk_strlen(y), mk_strlen(n))), mgr);
        expr_ref ax_l(mgr.mk_and(ax_l1, ax_l2), mgr);
        expr_ref ax_r(mgr.mk_and(ctx.mk_eq_atom(x,m), ctx.mk_eq_atom(y,n)), mgr);
        assert_implication(ax_l, ax_r);
    } else if (splitType == 2) {
        // Type 2: X cuts N.
        //   len(x) > len(m) || len(y) < len(n)
        if (!has_self_cut(x, n)) {
            expr_ref m_t2(mk_concat(m, t2), mgr);
            expr_ref t2_y(mk_concat(t2, y), mgr);

            expr ** ax_l_items = alloc_svect(expr*, 3);
            ax_l_items[0] = ctx.mk_eq_atom(concatAst1, concatAst2);

            expr ** ax_r_items = alloc_svect(expr*, 3);
            ax_r_items[0] = ctx.mk_eq_atom(x, m_t2);
            ax_r_items[1] = ctx.mk_eq_atom(t2_y, n);

            if (m_len_exists && x_len_exists) {
                ax_l_items[1] = ctx.mk_eq_atom(mk_strlen(x), mk_int(x_len));
                ax_l_items[2] = ctx.mk_eq_atom(mk_strlen(m), mk_int(m_len));
                rational x_sub_m = x_len - m_len;
                ax_r_items[2] = ctx.mk_eq_atom(mk_strlen(t2), mk_int(x_sub_m));
            } else {
                ax_l_items[1] = ctx.mk_eq_atom(mk_strlen(y), mk_int(y_len));
                ax_l_items[2] = ctx.mk_eq_atom(mk_strlen(n), mk_int(n_len));
                rational n_sub_y = n_len - y_len;
                ax_r_items[2] = ctx.mk_eq_atom(mk_strlen(t2), mk_int(n_sub_y));
            }

            expr_ref ax_l(mgr.mk_and(3, ax_l_items), mgr);
            expr_ref ax_r(mgr.mk_and(3, ax_r_items), mgr);

            // Cut Info
            add_cut_info_merge(t2, sLevel, x);
            add_cut_info_merge(t2, sLevel, n);

            assert_implication(ax_l, ax_r);
        } else {
            loopDetected = true;
            TRACE("t_str", tout << "AVOID LOOP: SKIPPED" << std::endl;);
            // TODO printCutVar(m, y);
        }
    } else if (splitType == -1) {
        // Here we don't really have a choice. We have no length information at all...
        expr ** or_item = alloc_svect(expr*, 3);
        expr ** and_item = alloc_svect(expr*, 20);
        int option = 0;
        int pos = 1;

        // break option 1: m cuts y
        // len(x) < len(m) || len(y) > len(n)
        if (!avoidLoopCut || !has_self_cut(m, y)) {
            // break down option 1-1
            expr * x_t1 = mk_concat(x, t1);
            expr * t1_n = mk_concat(t1, n);
            or_item[option] = ctx.mk_eq_atom(xorFlag, mk_int(option));
            and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(m, x_t1));
            and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(y, t1_n));

            expr_ref x_plus_t1(m_autil.mk_add(mk_strlen(x), mk_strlen(t1)), mgr);
            and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(mk_strlen(m), x_plus_t1));
            // These were crashing the solver because the integer theory
            // expects a constant on the right-hand side.
            // The things we want to assert here are len(m) > len(x) and len(y) > len(n).
            // We rewrite A > B as A-B > 0 and then as not(A-B <= 0),
            // and then, *because we aren't allowed to use subtraction*,
            // as not(A + -1*B <= 0)
            and_item[pos++] = ctx.mk_eq_atom(or_item[option],
                    mgr.mk_not(m_autil.mk_le(
                    		m_autil.mk_add(mk_strlen(m), m_autil.mk_mul(mk_int(-1), mk_strlen(x))),
							mk_int(0))) );
            and_item[pos++] = ctx.mk_eq_atom(or_item[option],
                    mgr.mk_not(m_autil.mk_le(
                    		m_autil.mk_add(mk_strlen(y),m_autil.mk_mul(mk_int(-1), mk_strlen(n))),
							mk_int(0))) );

            option++;

            add_cut_info_merge(t1, ctx.get_scope_level(), m);
            add_cut_info_merge(t1, ctx.get_scope_level(), y);
        } else {
            loopDetected = true;
            TRACE("t_str", tout << "AVOID LOOP: SKIPPED" << std::endl;);
            // TODO printCutVar(m, y);
        }

        // break option 2:
        // x = m || y = n
        if (!avoidLoopCut || !has_self_cut(x, n)) {
            // break down option 1-2
            expr * m_t2 = mk_concat(m, t2);
            expr * t2_y = mk_concat(t2, y);
            or_item[option] = ctx.mk_eq_atom(xorFlag, mk_int(option));
            and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(x, m_t2));
            and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(n, t2_y));


            expr_ref m_plus_t2(m_autil.mk_add(mk_strlen(m), mk_strlen(t2)), mgr);

            and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(mk_strlen(x), m_plus_t2));
            // want len(x) > len(m) and len(n) > len(y)
            and_item[pos++] = ctx.mk_eq_atom(or_item[option],
            		mgr.mk_not(m_autil.mk_le(
            				m_autil.mk_add(mk_strlen(x), m_autil.mk_mul(mk_int(-1), mk_strlen(m))),
							mk_int(0))) );
            and_item[pos++] = ctx.mk_eq_atom(or_item[option],
            		mgr.mk_not(m_autil.mk_le(
            				m_autil.mk_add(mk_strlen(n), m_autil.mk_mul(mk_int(-1), mk_strlen(y))),
							mk_int(0))) );


            option++;

            add_cut_info_merge(t2, ctx.get_scope_level(), x);
            add_cut_info_merge(t2, ctx.get_scope_level(), n);
        } else {
            loopDetected = true;
            TRACE("t_str", tout << "AVOID LOOP: SKIPPED" << std::endl;);
            // TODO printCutVar(x, n);
        }

        if (can_two_nodes_eq(x, m) && can_two_nodes_eq(y, n)) {
            or_item[option] = ctx.mk_eq_atom(xorFlag, mk_int(option));
            and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(x, m));
            and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(y, n));
            and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(mk_strlen(x), mk_strlen(m)));
            and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(mk_strlen(y), mk_strlen(n)));
            ++option;
        }

        if (option > 0) {
            if (option == 1) {
                and_item[0] = or_item[0];
            } else {
                and_item[0] = mgr.mk_or(option, or_item);
            }
            expr_ref premise(ctx.mk_eq_atom(concatAst1, concatAst2), mgr);
            expr_ref conclusion(mgr.mk_and(pos, and_item), mgr);
            assert_implication(premise, conclusion);
        } else {
            TRACE("t_str", tout << "STOP: no split option found for two EQ concats." << std::endl;);
        }
    } // (splitType == -1)
}

/*************************************************************
 * Type 2: concat(x, y) = concat(m, "str")
 *************************************************************/
bool theory_str::is_concat_eq_type2(expr * concatAst1, expr * concatAst2) {
	expr * v1_arg0 = to_app(concatAst1)->get_arg(0);
	expr * v1_arg1 = to_app(concatAst1)->get_arg(1);
	expr * v2_arg0 = to_app(concatAst2)->get_arg(0);
	expr * v2_arg1 = to_app(concatAst2)->get_arg(1);

	if ((!m_strutil.is_string(v1_arg0)) && m_strutil.is_string(v1_arg1)
			&& (!m_strutil.is_string(v2_arg0)) && (!m_strutil.is_string(v2_arg1))) {
		return true;
	} else if ((!m_strutil.is_string(v2_arg0)) && m_strutil.is_string(v2_arg1)
			&& (!m_strutil.is_string(v1_arg0)) && (!m_strutil.is_string(v1_arg1))) {
		return true;
	} else {
		return false;
	}
}

void theory_str::process_concat_eq_type2(expr * concatAst1, expr * concatAst2) {
	ast_manager & mgr = get_manager();
	context & ctx = get_context();
	TRACE("t_str_detail", tout << "process_concat_eq TYPE 2" << std::endl
			<< "concatAst1 = " << mk_ismt2_pp(concatAst1, mgr) << std::endl
			<< "concatAst2 = " << mk_ismt2_pp(concatAst2, mgr) << std::endl;
	);

	if (!is_concat(to_app(concatAst1))) {
		TRACE("t_str_detail", tout << "concatAst1 is not a concat function" << std::endl;);
		return;
	}
	if (!is_concat(to_app(concatAst2))) {
		TRACE("t_str_detail", tout << "concatAst2 is not a concat function" << std::endl;);
		return;
	}

	expr * x = NULL;
	expr * y = NULL;
	expr * strAst = NULL;
	expr * m = NULL;

	expr * v1_arg0 = to_app(concatAst1)->get_arg(0);
	expr * v1_arg1 = to_app(concatAst1)->get_arg(1);
	expr * v2_arg0 = to_app(concatAst2)->get_arg(0);
	expr * v2_arg1 = to_app(concatAst2)->get_arg(1);

	if (m_strutil.is_string(v1_arg1) && !m_strutil.is_string(v2_arg1)) {
		m = v1_arg0;
		strAst = v1_arg1;
		x = v2_arg0;
		y = v2_arg1;
	} else {
		m = v2_arg0;
		strAst = v2_arg1;
		x = v1_arg0;
		y = v1_arg1;
	}

	std::string strValue = m_strutil.get_string_constant_value(strAst);

	rational x_len, y_len, m_len, str_len;
	bool x_len_exists = get_len_value(x, x_len);
	bool y_len_exists = get_len_value(y, y_len);
	bool m_len_exists = get_len_value(m, m_len);
	bool str_len_exists = true;
	str_len = rational((unsigned)(strValue.length()));

	// setup

	expr * xorFlag = NULL;
	expr * temp1 = NULL;
	std::pair<expr*, expr*> key1(concatAst1, concatAst2);
	std::pair<expr*, expr*> key2(concatAst2, concatAst1);

	// check the entries in this map to make sure they're still in scope
	// before we use them.

	std::map<std::pair<expr*,expr*>, std::map<int, expr*> >::iterator entry1 = varForBreakConcat.find(key1);
	std::map<std::pair<expr*,expr*>, std::map<int, expr*> >::iterator entry2 = varForBreakConcat.find(key2);

    // prevent checking scope for the XOR term, as it's always in the same scope as the split var

	bool entry1InScope;
	if (entry1 == varForBreakConcat.end()) {
	    entry1InScope = false;
	} else {
	    if (internal_variable_set.find((entry1->second)[0]) == internal_variable_set.end()
	            /*|| internal_variable_set.find((entry1->second)[1]) == internal_variable_set.end()*/
	            ) {
	        entry1InScope = false;
	    } else {
	        entry1InScope = true;
	    }
	}

	bool entry2InScope;
	if (entry2 == varForBreakConcat.end()) {
	    entry2InScope = false;
	} else {
	    if (internal_variable_set.find((entry2->second)[0]) == internal_variable_set.end()
	            /*|| internal_variable_set.find((entry2->second)[1]) == internal_variable_set.end()*/
	            ) {
	        entry2InScope = false;
	    } else {
	        entry2InScope = true;
	    }
	}

	TRACE("t_str_detail", tout << "entry 1 " << (entry1InScope ? "in scope" : "not in scope") << std::endl
	        << "entry 2 " << (entry2InScope ? "in scope" : "not in scope") << std::endl;);


	if (!entry1InScope && !entry2InScope) {
		temp1 = mk_nonempty_str_var();
		xorFlag = mk_internal_xor_var();
		varForBreakConcat[key1][0] = temp1;
		varForBreakConcat[key1][1] = xorFlag;
	} else {
		if (entry1InScope) {
			temp1 = varForBreakConcat[key1][0];
			xorFlag = varForBreakConcat[key1][1];
		} else if (entry2InScope) {
			temp1 = varForBreakConcat[key2][0];
			xorFlag = varForBreakConcat[key2][1];
		}
		// TODO refresh xorFlag?
		refresh_theory_var(temp1);
	}

	int splitType = -1;
	if (x_len_exists && m_len_exists) {
		if (x_len < m_len)
			splitType = 0;
		else if (x_len == m_len)
			splitType = 1;
		else
			splitType = 2;
	}
	if (splitType == -1 && y_len_exists && str_len_exists) {
		if (y_len > str_len)
			splitType = 0;
		else if (y_len == str_len)
			splitType = 1;
		else
			splitType = 2;
	}

	TRACE("t_str_detail", tout << "Split type " << splitType << std::endl;);

	// Provide fewer split options when length information is available.

	if (splitType == 0) {
	    // M cuts Y
	    //   |  x  |      y     |
        //   |    m   |   str   |
	    expr_ref temp1_strAst(mk_concat(temp1, strAst), mgr);
	    if (can_two_nodes_eq(y, temp1_strAst)) {
	        if (!avoidLoopCut || !(has_self_cut(m, y))) {
	            // break down option 2-1
	            expr ** l_items = alloc_svect(expr*, 3);
	            l_items[0] = ctx.mk_eq_atom(concatAst1, concatAst2);

	            expr ** r_items = alloc_svect(expr*, 3);
	            expr_ref x_temp1(mk_concat(x, temp1), mgr);
	            r_items[0] = ctx.mk_eq_atom(m, x_temp1);
	            r_items[1] = ctx.mk_eq_atom(y, temp1_strAst);

	            if (x_len_exists && m_len_exists) {
	                l_items[1] = ctx.mk_eq_atom(mk_strlen(x), mk_int(x_len));
	                l_items[2] = ctx.mk_eq_atom(mk_strlen(m), mk_int(m_len));
	                rational m_sub_x = (m_len - x_len);
	                r_items[2] = ctx.mk_eq_atom(mk_strlen(temp1), mk_int(m_sub_x));
	            } else {
	                l_items[1] = ctx.mk_eq_atom(mk_strlen(y), mk_int(y_len));
	                l_items[2] = ctx.mk_eq_atom(mk_strlen(strAst), mk_int(str_len));
	                rational y_sub_str = (y_len - str_len);
	                r_items[2] = ctx.mk_eq_atom(mk_strlen(temp1), mk_int(y_sub_str));
	            }

	            expr_ref ax_l(mgr.mk_and(3, l_items), mgr);
	            expr_ref ax_r(mgr.mk_and(3, r_items), mgr);

	            add_cut_info_merge(temp1, sLevel, y);
	            add_cut_info_merge(temp1, sLevel, m);

	            assert_implication(ax_l, ax_r);
	        } else {
	            loopDetected = true;
	            TRACE("t_str", tout << "AVOID LOOP: SKIP" << std::endl;);
	            // TODO printCutVar(m, y);
	        }
	    }
	} else if (splitType == 1) {
	    //   |   x   |    y    |
	    //   |   m   |   str   |
	    expr_ref ax_l1(ctx.mk_eq_atom(concatAst1, concatAst2), mgr);
	    expr_ref ax_l2(mgr.mk_or(
	            ctx.mk_eq_atom(mk_strlen(x), mk_strlen(m)),
	            ctx.mk_eq_atom(mk_strlen(y), mk_strlen(strAst))), mgr);
	    expr_ref ax_l(mgr.mk_and(ax_l1, ax_l2), mgr);
	    expr_ref ax_r(mgr.mk_and(ctx.mk_eq_atom(x, m), ctx.mk_eq_atom(y, strAst)), mgr);
	    assert_implication(ax_l, ax_r);
	} else if (splitType == 2) {
	    // m cut y,
	    //    |   x   |  y  |
	    //    | m |   str   |
	    rational lenDelta;
	    expr ** l_items = alloc_svect(expr*, 3);
	    int l_count = 0;
	    l_items[0] = ctx.mk_eq_atom(concatAst1, concatAst2);
	    if (x_len_exists && m_len_exists) {
	        l_items[1] = ctx.mk_eq_atom(mk_strlen(x), mk_int(x_len));
	        l_items[2] = ctx.mk_eq_atom(mk_strlen(m), mk_int(m_len));
	        l_count = 3;
	        lenDelta = x_len - m_len;
	    } else {
	        l_items[1] = ctx.mk_eq_atom(mk_strlen(y), mk_int(y_len));
	        l_count = 2;
	        lenDelta = str_len - y_len;
	    }
	    TRACE("t_str",
	            tout
	                << "xLen? " << (x_len_exists ? "yes" : "no") << std::endl
	                << "mLen? " << (m_len_exists ? "yes" : "no") << std::endl
	                << "yLen? " << (y_len_exists ? "yes" : "no") << std::endl
	                << "xLen = " << x_len.to_string() << std::endl
	                << "yLen = " << y_len.to_string() << std::endl
	                << "mLen = " << m_len.to_string() << std::endl
	                << "strLen = " << str_len.to_string() << std::endl
	                << "lenDelta = " << lenDelta.to_string() << std::endl
	                << "strValue = \"" << strValue << "\" (len=" << strValue.length() << ")" << std::endl
	                 ;
	            );

	    TRACE("t_str", tout << "*** MARKER 1 ***" << std::endl;);
	    std::string part1Str = strValue.substr(0, lenDelta.get_unsigned());
	    TRACE("t_str", tout << "*** MARKER 2 ***" << std::endl;);
	    std::string part2Str = strValue.substr(lenDelta.get_unsigned(), strValue.length() - lenDelta.get_unsigned());
	    TRACE("t_str", tout << "*** MARKER 3 ***" << std::endl;);

	    expr_ref prefixStr(m_strutil.mk_string(part1Str), mgr);
	    expr_ref x_concat(mk_concat(m, prefixStr), mgr);
	    expr_ref cropStr(m_strutil.mk_string(part2Str), mgr);

	    if (can_two_nodes_eq(x, x_concat) && can_two_nodes_eq(y, cropStr)) {
	        expr ** r_items = alloc_svect(expr*, 2);
	        r_items[0] = ctx.mk_eq_atom(x, x_concat);
	        r_items[1] = ctx.mk_eq_atom(y, cropStr);
	        expr_ref ax_l(mgr.mk_and(l_count, l_items), mgr);
	        expr_ref ax_r(mgr.mk_and(2, r_items), mgr);

	        assert_implication(ax_l, ax_r);
	    } else {
	        // negate! It's impossible to split str with these lengths
	        TRACE("t_str", tout << "CONFLICT: Impossible to split str with these lengths." << std::endl;);
	        expr_ref ax_l(mgr.mk_and(l_count, l_items), mgr);
	        assert_axiom(mgr.mk_not(ax_l));
	    }
	} else {
		// Split type -1: no idea about the length...
		int optionTotal = 2 + strValue.length();
		expr ** or_item = alloc_svect(expr*, optionTotal);
		expr ** and_item = alloc_svect(expr*, (1 + 6 + 4 * (strValue.length() + 1)));
		int option = 0;
		int pos = 1;

		expr_ref temp1_strAst(mk_concat(temp1, strAst), mgr);

		// m cuts y
		if (can_two_nodes_eq(y, temp1_strAst)) {
			if (!avoidLoopCut || !has_self_cut(m, y)) {
				// break down option 2-1
				or_item[option] = ctx.mk_eq_atom(xorFlag, mk_int(option));
				expr_ref x_temp1(mk_concat(x, temp1), mgr);
				and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(m, x_temp1));
				and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(y, temp1_strAst));

				and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(mk_strlen(m),
						m_autil.mk_add(mk_strlen(x), mk_strlen(temp1))));

				++option;
				add_cut_info_merge(temp1, ctx.get_scope_level(), y);
				add_cut_info_merge(temp1, ctx.get_scope_level(), m);
			} else {
				loopDetected = true;
				TRACE("t_str", tout << "AVOID LOOP: SKIPPED" << std::endl;);
				// TODO printCutVar(m, y)
			}
		}

		for (int i = 0; i <= (int)strValue.size(); ++i) {
			std::string part1Str = strValue.substr(0, i);
			std::string part2Str = strValue.substr(i, strValue.size() - i);
			expr_ref prefixStr(m_strutil.mk_string(part1Str), mgr);
			expr_ref x_concat(mk_concat(m, prefixStr), mgr);
			expr_ref cropStr(m_strutil.mk_string(part2Str), mgr);
			if (can_two_nodes_eq(x, x_concat) && can_two_nodes_eq(y, cropStr)) {
				// break down option 2-2
				or_item[option] = ctx.mk_eq_atom(xorFlag, mk_int(option));
				and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(x, x_concat));
				and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(y, cropStr));
				and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(mk_strlen(y), mk_int(part2Str.length())));
				++option;
			}
		}

		if (option > 0) {
			if (option == 1) {
				and_item[0] = or_item[0];
			} else {
				and_item[0] = mgr.mk_or(option, or_item);
			}
			expr_ref implyR(mgr.mk_and(pos, and_item), mgr);
			assert_implication(ctx.mk_eq_atom(concatAst1, concatAst2), implyR);
		} else {
			TRACE("t_str", tout << "STOP: Should not split two EQ concats." << std::endl;);
		}
	} // (splitType == -1)
}

/*************************************************************
 * Type 3: concat(x, y) = concat("str", n)
 *************************************************************/
bool theory_str::is_concat_eq_type3(expr * concatAst1, expr * concatAst2) {
    expr * v1_arg0 = to_app(concatAst1)->get_arg(0);
    expr * v1_arg1 = to_app(concatAst1)->get_arg(1);
    expr * v2_arg0 = to_app(concatAst2)->get_arg(0);
    expr * v2_arg1 = to_app(concatAst2)->get_arg(1);

    if (m_strutil.is_string(v1_arg0) && (!m_strutil.is_string(v1_arg1))
            && (!m_strutil.is_string(v2_arg0)) && (!m_strutil.is_string(v2_arg1))) {
        return true;
    } else if (m_strutil.is_string(v2_arg0) && (!m_strutil.is_string(v2_arg1))
            && (!m_strutil.is_string(v1_arg0)) && (!m_strutil.is_string(v1_arg1))) {
        return true;
    } else {
        return false;
    }
}

void theory_str::process_concat_eq_type3(expr * concatAst1, expr * concatAst2) {
    ast_manager & mgr = get_manager();
    context & ctx = get_context();
    TRACE("t_str_detail", tout << "process_concat_eq TYPE 3" << std::endl
            << "concatAst1 = " << mk_ismt2_pp(concatAst1, mgr) << std::endl
            << "concatAst2 = " << mk_ismt2_pp(concatAst2, mgr) << std::endl;
    );

    if (!is_concat(to_app(concatAst1))) {
        TRACE("t_str_detail", tout << "concatAst1 is not a concat function" << std::endl;);
        return;
    }
    if (!is_concat(to_app(concatAst2))) {
        TRACE("t_str_detail", tout << "concatAst2 is not a concat function" << std::endl;);
        return;
    }

    expr * v1_arg0 = to_app(concatAst1)->get_arg(0);
    expr * v1_arg1 = to_app(concatAst1)->get_arg(1);
    expr * v2_arg0 = to_app(concatAst2)->get_arg(0);
    expr * v2_arg1 = to_app(concatAst2)->get_arg(1);

    expr * x = NULL;
    expr * y = NULL;
    expr * strAst = NULL;
    expr * n = NULL;

    if (m_strutil.is_string(v1_arg0) && !m_strutil.is_string(v2_arg0)) {
        strAst = v1_arg0;
        n = v1_arg1;
        x = v2_arg0;
        y = v2_arg1;
    } else {
        strAst = v2_arg0;
        n = v2_arg1;
        x = v1_arg0;
        y = v1_arg1;
    }

    std::string strValue = m_strutil.get_string_constant_value(strAst);

    rational x_len, y_len, str_len, n_len;
    bool x_len_exists = get_len_value(x, x_len);
    bool y_len_exists = get_len_value(y, y_len);
    str_len = rational((unsigned)(strValue.length()));
    bool n_len_exists = get_len_value(n, n_len);

    expr_ref xorFlag(mgr);
    expr_ref temp1(mgr);
    std::pair<expr*, expr*> key1(concatAst1, concatAst2);
    std::pair<expr*, expr*> key2(concatAst2, concatAst1);

    // check the entries in this map to make sure they're still in scope
    // before we use them.

    std::map<std::pair<expr*,expr*>, std::map<int, expr*> >::iterator entry1 = varForBreakConcat.find(key1);
    std::map<std::pair<expr*,expr*>, std::map<int, expr*> >::iterator entry2 = varForBreakConcat.find(key2);

    bool entry1InScope;
    if (entry1 == varForBreakConcat.end()) {
        entry1InScope = false;
    } else {
        if (internal_variable_set.find((entry1->second)[0]) == internal_variable_set.end()
                /* || internal_variable_set.find((entry1->second)[1]) == internal_variable_set.end() */) {
            entry1InScope = false;
        } else {
            entry1InScope = true;
        }
    }

    bool entry2InScope;
    if (entry2 == varForBreakConcat.end()) {
        entry2InScope = false;
    } else {
        if (internal_variable_set.find((entry2->second)[0]) == internal_variable_set.end()
                /* || internal_variable_set.find((entry2->second)[1]) == internal_variable_set.end() */) {
            entry2InScope = false;
        } else {
            entry2InScope = true;
        }
    }

    TRACE("t_str_detail", tout << "entry 1 " << (entry1InScope ? "in scope" : "not in scope") << std::endl
            << "entry 2 " << (entry2InScope ? "in scope" : "not in scope") << std::endl;);


    if (!entry1InScope && !entry2InScope) {
        temp1 = mk_nonempty_str_var();
        xorFlag = mk_internal_xor_var();

        varForBreakConcat[key1][0] = temp1;
        varForBreakConcat[key1][1] = xorFlag;
    } else {
        if (entry1InScope) {
            temp1 = varForBreakConcat[key1][0];
            xorFlag = varForBreakConcat[key1][1];
        } else if (varForBreakConcat.find(key2) != varForBreakConcat.end()) {
            temp1 = varForBreakConcat[key2][0];
            xorFlag = varForBreakConcat[key2][1];
        }
        refresh_theory_var(temp1);
    }



    int splitType = -1;
    if (x_len_exists) {
        if (x_len < str_len)
            splitType = 0;
        else if (x_len == str_len)
            splitType = 1;
        else
            splitType = 2;
    }
    if (splitType == -1 && y_len_exists && n_len_exists) {
        if (y_len > n_len)
            splitType = 0;
        else if (y_len == n_len)
            splitType = 1;
        else
            splitType = 2;
    }

    TRACE("t_str_detail", tout << "Split type " << splitType << std::endl;);

    // Provide fewer split options when length information is available.
    if (splitType == 0) {
        //   |   x   |    y     |
        //   |  str     |   n   |
        expr_ref_vector litems(mgr);
        litems.push_back(ctx.mk_eq_atom(concatAst1, concatAst2));
        rational prefixLen;
        if (!x_len_exists) {
            prefixLen = str_len - (y_len - n_len);
            litems.push_back(ctx.mk_eq_atom(mk_strlen(y), mk_int(y_len)));
            litems.push_back(ctx.mk_eq_atom(mk_strlen(n), mk_int(n_len)));
        } else {
            prefixLen = x_len;
            litems.push_back(ctx.mk_eq_atom(mk_strlen(x), mk_int(x_len)));
        }
        std::string prefixStr = strValue.substr(0, prefixLen.get_unsigned());
        rational str_sub_prefix = str_len - prefixLen;
        std::string suffixStr = strValue.substr(prefixLen.get_unsigned(), str_sub_prefix.get_unsigned());
        expr_ref prefixAst(m_strutil.mk_string(prefixStr), mgr);
        expr_ref suffixAst(m_strutil.mk_string(suffixStr), mgr);
        expr_ref ax_l(mgr.mk_and(litems.size(), litems.c_ptr()), mgr);

        expr_ref suf_n_concat(mk_concat(suffixAst, n), mgr);
        if (can_two_nodes_eq(x, prefixAst) && can_two_nodes_eq(y, suf_n_concat)) {
            expr_ref_vector r_items(mgr);
            r_items.push_back(ctx.mk_eq_atom(x, prefixAst));
            r_items.push_back(ctx.mk_eq_atom(y, suf_n_concat));
            assert_implication(ax_l, mk_and(r_items));
        } else {
            // negate! It's impossible to split str with these lengths
            TRACE("t_str", tout << "CONFLICT: Impossible to split str with these lengths." << std::endl;);
            assert_axiom(mgr.mk_not(ax_l));
        }
    }
    else if (splitType == 1) {
        expr_ref ax_l1(ctx.mk_eq_atom(concatAst1, concatAst2), mgr);
        expr_ref ax_l2(mgr.mk_or(
                ctx.mk_eq_atom(mk_strlen(x), mk_strlen(strAst)),
                ctx.mk_eq_atom(mk_strlen(y), mk_strlen(n))), mgr);
        expr_ref ax_l(mgr.mk_and(ax_l1, ax_l2), mgr);
        expr_ref ax_r(mgr.mk_and(ctx.mk_eq_atom(x, strAst), ctx.mk_eq_atom(y, n)), mgr);
        assert_implication(ax_l, ax_r);
    }
    else if (splitType == 2) {
        //   |   x        |    y     |
        //   |  str   |       n      |
        expr_ref_vector litems(mgr);
        litems.push_back(ctx.mk_eq_atom(concatAst1, concatAst2));
        rational tmpLen;
        if (!x_len_exists) {
            tmpLen = n_len - y_len;
            litems.push_back(ctx.mk_eq_atom(mk_strlen(y), mk_int(y_len)));
            litems.push_back(ctx.mk_eq_atom(mk_strlen(n), mk_int(n_len)));
        } else {
            tmpLen = x_len - str_len;
            litems.push_back(ctx.mk_eq_atom(mk_strlen(x), mk_int(x_len)));
        }
        expr_ref ax_l(mgr.mk_and(litems.size(), litems.c_ptr()), mgr);

        expr_ref str_temp1(mk_concat(strAst, temp1), mgr);
        expr_ref temp1_y(mk_concat(temp1, y), mgr);

        if (can_two_nodes_eq(x, str_temp1)) {
            if (!avoidLoopCut || !(has_self_cut(x, n))) {
                expr_ref_vector r_items(mgr);
                r_items.push_back(ctx.mk_eq_atom(x, str_temp1));
                r_items.push_back(ctx.mk_eq_atom(n, temp1_y));
                r_items.push_back(ctx.mk_eq_atom(mk_strlen(temp1), mk_int(tmpLen)));
                expr_ref ax_r(mk_and(r_items), mgr);

                //Cut Info
                add_cut_info_merge(temp1, sLevel, x);
                add_cut_info_merge(temp1, sLevel, n);

                assert_implication(ax_l, ax_r);
            } else {
                loopDetected = true;
                TRACE("t_str", tout << "AVOID LOOP: SKIPPED" << std::endl;);
                // TODO printCutVar(x, n);
            }
        }
        //    else {
        //      // negate! It's impossible to split str with these lengths
        //      __debugPrint(logFile, "[Conflict] Negate! It's impossible to split str with these lengths @ %d.\n", __LINE__);
        //      addAxiom(t, Z3_mk_not(ctx, ax_l), __LINE__);
        //    }
    }
    else {
        // Split type -1. We know nothing about the length...

        expr_ref_vector or_item(mgr);
        unsigned option = 0;
        expr_ref_vector and_item(mgr);
        int pos = 1;
        for (int i = 0; i <= (int) strValue.size(); i++) {
            std::string part1Str = strValue.substr(0, i);
            std::string part2Str = strValue.substr(i, strValue.size() - i);
            expr_ref cropStr(m_strutil.mk_string(part1Str), mgr);
            expr_ref suffixStr(m_strutil.mk_string(part2Str), mgr);
            expr_ref y_concat(mk_concat(suffixStr, n), mgr);

            if (can_two_nodes_eq(x, cropStr) && can_two_nodes_eq(y, y_concat)) {
                // break down option 3-1
                expr_ref x_eq_str(ctx.mk_eq_atom(x, cropStr), mgr);
                or_item.push_back(ctx.mk_eq_atom(xorFlag, mk_int(option)));
                and_item.push_back(ctx.mk_eq_atom(or_item.get(option), x_eq_str)); ++pos;
                and_item.push_back(ctx.mk_eq_atom(or_item.get(option), ctx.mk_eq_atom(y, y_concat)));

                and_item.push_back(ctx.mk_eq_atom(or_item.get(option), ctx.mk_eq_atom(mk_strlen(x), mk_strlen(cropStr)))); ++pos;
                //        and_item[pos++] = Z3_mk_eq(ctx, or_item[option], Z3_mk_eq(ctx, mk_length(t, y), mk_length(t, y_concat)));

                // adding length constraint for _ = constStr seems slowing things down.
                option++;
            }
        }

        expr_ref strAst_temp1(mk_concat(strAst, temp1), mgr);


        //--------------------------------------------------------
        // x cut n
        //--------------------------------------------------------
        if (can_two_nodes_eq(x, strAst_temp1)) {
            if (!avoidLoopCut || !(has_self_cut(x, n))) {
                // break down option 3-2
                or_item.push_back(ctx.mk_eq_atom(xorFlag, mk_int(option)));

                expr_ref temp1_y(mk_concat(temp1, y), mgr);
                and_item.push_back(ctx.mk_eq_atom(or_item.get(option), ctx.mk_eq_atom(x, strAst_temp1))); ++pos;
                and_item.push_back(ctx.mk_eq_atom(or_item.get(option), ctx.mk_eq_atom(n, temp1_y))); ++pos;

                and_item.push_back(ctx.mk_eq_atom(or_item.get(option), ctx.mk_eq_atom(mk_strlen(x),
                        m_autil.mk_add(mk_strlen(strAst), mk_strlen(temp1)) )) ); ++pos;
                option++;

                add_cut_info_merge(temp1, sLevel, x);
                add_cut_info_merge(temp1, sLevel, n);
            } else {
                loopDetected = true;
                TRACE("t_str", tout << "AVOID LOOP: SKIPPED." << std::endl;);
                // TODO printCutVAR(x, n)
            }
        }


        if (option > 0) {
            if (option == 1) {
                and_item.push_back(or_item.get(0));
            } else {
                and_item.push_back(mk_or(or_item));
            }
            expr_ref implyR(mk_and(and_item), mgr);
            assert_implication(ctx.mk_eq_atom(concatAst1, concatAst2), implyR);
        } else {
            TRACE("t_str", tout << "STOP: should not split two eq. concats" << std::endl;);
        }
    }

}

/*************************************************************
 * Type 4: concat("str1", y) = concat("str2", n)
 *************************************************************/
bool theory_str::is_concat_eq_type4(expr * concatAst1, expr * concatAst2) {
    expr * v1_arg0 = to_app(concatAst1)->get_arg(0);
    expr * v1_arg1 = to_app(concatAst1)->get_arg(1);
    expr * v2_arg0 = to_app(concatAst2)->get_arg(0);
    expr * v2_arg1 = to_app(concatAst2)->get_arg(1);

    if (m_strutil.is_string(v1_arg0) && (!m_strutil.is_string(v1_arg1))
            && m_strutil.is_string(v2_arg0) && (!m_strutil.is_string(v2_arg1))) {
      return true;
    } else {
      return false;
    }
}

void theory_str::process_concat_eq_type4(expr * concatAst1, expr * concatAst2) {
    ast_manager & mgr = get_manager();
    context & ctx = get_context();
    TRACE("t_str_detail", tout << "process_concat_eq TYPE 4" << std::endl
            << "concatAst1 = " << mk_ismt2_pp(concatAst1, mgr) << std::endl
            << "concatAst2 = " << mk_ismt2_pp(concatAst2, mgr) << std::endl;
    );

    if (!is_concat(to_app(concatAst1))) {
        TRACE("t_str_detail", tout << "concatAst1 is not a concat function" << std::endl;);
        return;
    }
    if (!is_concat(to_app(concatAst2))) {
        TRACE("t_str_detail", tout << "concatAst2 is not a concat function" << std::endl;);
        return;
    }

    expr * v1_arg0 = to_app(concatAst1)->get_arg(0);
    expr * v1_arg1 = to_app(concatAst1)->get_arg(1);
    expr * v2_arg0 = to_app(concatAst2)->get_arg(0);
    expr * v2_arg1 = to_app(concatAst2)->get_arg(1);

    expr * str1Ast = v1_arg0;
    expr * y = v1_arg1;
    expr * str2Ast = v2_arg0;
    expr * n = v2_arg1;

    const char *tmp = 0;
    m_strutil.is_string(str1Ast, &tmp);
    std::string str1Value(tmp);
    m_strutil.is_string(str2Ast, &tmp);
    std::string str2Value(tmp);

    int str1Len = str1Value.length();
    int str2Len = str2Value.length();

    int commonLen = (str1Len > str2Len) ? str2Len : str1Len;
    if (str1Value.substr(0, commonLen) != str2Value.substr(0, commonLen)) {
        TRACE("t_str_detail", tout << "Conflict: " << mk_ismt2_pp(concatAst1, mgr)
                << " has no common prefix with " << mk_ismt2_pp(concatAst2, mgr) << std::endl;);
        expr_ref toNegate(mgr.mk_not(ctx.mk_eq_atom(concatAst1, concatAst2)), mgr);
        assert_axiom(toNegate);
        return;
    } else {
        if (str1Len > str2Len) {
            std::string deltaStr = str1Value.substr(str2Len, str1Len - str2Len);
            expr_ref tmpAst(mk_concat(m_strutil.mk_string(deltaStr), y), mgr);
            if (!in_same_eqc(tmpAst, n)) {
                // break down option 4-1
                expr_ref implyR(ctx.mk_eq_atom(n, tmpAst), mgr);
                assert_implication(ctx.mk_eq_atom(concatAst1, concatAst2), implyR);
            }
        } else if (str1Len == str2Len) {
            if (!in_same_eqc(n, y)) {
                //break down option 4-2
                expr_ref implyR(ctx.mk_eq_atom(n, y), mgr);
                assert_implication(ctx.mk_eq_atom(concatAst1, concatAst2), implyR);
            }
        } else {
            std::string deltaStr = str2Value.substr(str1Len, str2Len - str1Len);
            expr_ref tmpAst(mk_concat(m_strutil.mk_string(deltaStr), n), mgr);
            if (!in_same_eqc(y, tmpAst)) {
                //break down option 4-3
                expr_ref implyR(ctx.mk_eq_atom(y, tmpAst), mgr);
                assert_implication(ctx.mk_eq_atom(concatAst1, concatAst2), implyR);
            }
        }
    }
}

/*************************************************************
 *  case 5: concat(x, "str1") = concat(m, "str2")
 *************************************************************/
bool theory_str::is_concat_eq_type5(expr * concatAst1, expr * concatAst2) {
    expr * v1_arg0 = to_app(concatAst1)->get_arg(0);
    expr * v1_arg1 = to_app(concatAst1)->get_arg(1);
    expr * v2_arg0 = to_app(concatAst2)->get_arg(0);
    expr * v2_arg1 = to_app(concatAst2)->get_arg(1);

    if ((!m_strutil.is_string(v1_arg0)) && m_strutil.is_string(v1_arg1)
            && (!m_strutil.is_string(v2_arg0)) && m_strutil.is_string(v2_arg1)) {
        return true;
    } else {
        return false;
    }
}

void theory_str::process_concat_eq_type5(expr * concatAst1, expr * concatAst2) {
    ast_manager & mgr = get_manager();
    context & ctx = get_context();
    TRACE("t_str_detail", tout << "process_concat_eq TYPE 5" << std::endl
            << "concatAst1 = " << mk_ismt2_pp(concatAst1, mgr) << std::endl
            << "concatAst2 = " << mk_ismt2_pp(concatAst2, mgr) << std::endl;
    );

    if (!is_concat(to_app(concatAst1))) {
        TRACE("t_str_detail", tout << "concatAst1 is not a concat function" << std::endl;);
        return;
    }
    if (!is_concat(to_app(concatAst2))) {
        TRACE("t_str_detail", tout << "concatAst2 is not a concat function" << std::endl;);
        return;
    }

    expr * v1_arg0 = to_app(concatAst1)->get_arg(0);
    expr * v1_arg1 = to_app(concatAst1)->get_arg(1);
    expr * v2_arg0 = to_app(concatAst2)->get_arg(0);
    expr * v2_arg1 = to_app(concatAst2)->get_arg(1);

    expr * x = v1_arg0;
    expr * str1Ast = v1_arg1;
    expr * m = v2_arg0;
    expr * str2Ast = v2_arg1;

    const char *tmp = 0;
    m_strutil.is_string(str1Ast, &tmp);
    std::string str1Value(tmp);
    m_strutil.is_string(str2Ast, &tmp);
    std::string str2Value(tmp);

    int str1Len = str1Value.length();
    int str2Len = str2Value.length();

    int cLen = (str1Len > str2Len) ? str2Len : str1Len;
    if (str1Value.substr(str1Len - cLen, cLen) != str2Value.substr(str2Len - cLen, cLen)) {
        TRACE("t_str_detail", tout << "Conflict: " << mk_ismt2_pp(concatAst1, mgr)
                << " has no common suffix with " << mk_ismt2_pp(concatAst2, mgr) << std::endl;);
        expr_ref toNegate(mgr.mk_not(ctx.mk_eq_atom(concatAst1, concatAst2)), mgr);
        assert_axiom(toNegate);
        return;
    } else {
        if (str1Len > str2Len) {
            std::string deltaStr = str1Value.substr(0, str1Len - str2Len);
            expr_ref x_deltaStr(mk_concat(x, m_strutil.mk_string(deltaStr)), mgr);
            if (!in_same_eqc(m, x_deltaStr)) {
                expr_ref implyR(ctx.mk_eq_atom(m, x_deltaStr), mgr);
                assert_implication(ctx.mk_eq_atom(concatAst1, concatAst2), implyR);
            }
        } else if (str1Len == str2Len) {
            // test
            if (!in_same_eqc(x, m)) {
                expr_ref implyR(ctx.mk_eq_atom(x, m), mgr);
                assert_implication(ctx.mk_eq_atom(concatAst1, concatAst2), implyR);
            }
        } else {
            std::string deltaStr = str2Value.substr(0, str2Len - str1Len);
            expr_ref m_deltaStr(mk_concat(m, m_strutil.mk_string(deltaStr)), mgr);
            if (!in_same_eqc(x, m_deltaStr)) {
                expr_ref implyR(ctx.mk_eq_atom(x, m_deltaStr), mgr);
                assert_implication(ctx.mk_eq_atom(concatAst1, concatAst2), implyR);
            }
        }
    }
}

/*************************************************************
 *  case 6: concat("str1", y) = concat(m, "str2")
 *************************************************************/
bool theory_str::is_concat_eq_type6(expr * concatAst1, expr * concatAst2) {
    expr * v1_arg0 = to_app(concatAst1)->get_arg(0);
    expr * v1_arg1 = to_app(concatAst1)->get_arg(1);
    expr * v2_arg0 = to_app(concatAst2)->get_arg(0);
    expr * v2_arg1 = to_app(concatAst2)->get_arg(1);

    if (m_strutil.is_string(v1_arg0) && (!m_strutil.is_string(v1_arg1))
            && (!m_strutil.is_string(v2_arg0)) && m_strutil.is_string(v2_arg1)) {
        return true;
    } else if (m_strutil.is_string(v2_arg0) && (!m_strutil.is_string(v2_arg1))
            && (!m_strutil.is_string(v1_arg0)) && m_strutil.is_string(v1_arg1)) {
        return true;
    } else {
        return false;
    }
}

void theory_str::process_concat_eq_type6(expr * concatAst1, expr * concatAst2) {
    ast_manager & mgr = get_manager();
    context & ctx = get_context();
    TRACE("t_str_detail", tout << "process_concat_eq TYPE 6" << std::endl
            << "concatAst1 = " << mk_ismt2_pp(concatAst1, mgr) << std::endl
            << "concatAst2 = " << mk_ismt2_pp(concatAst2, mgr) << std::endl;
    );

    if (!is_concat(to_app(concatAst1))) {
        TRACE("t_str_detail", tout << "concatAst1 is not a concat function" << std::endl;);
        return;
    }
    if (!is_concat(to_app(concatAst2))) {
        TRACE("t_str_detail", tout << "concatAst2 is not a concat function" << std::endl;);
        return;
    }

    expr * v1_arg0 = to_app(concatAst1)->get_arg(0);
    expr * v1_arg1 = to_app(concatAst1)->get_arg(1);
    expr * v2_arg0 = to_app(concatAst2)->get_arg(0);
    expr * v2_arg1 = to_app(concatAst2)->get_arg(1);


    expr * str1Ast = NULL;
    expr * y = NULL;
    expr * m = NULL;
    expr * str2Ast = NULL;

    if (m_strutil.is_string(v1_arg0)) {
        str1Ast = v1_arg0;
        y = v1_arg1;
        m = v2_arg0;
        str2Ast = v2_arg1;
    } else {
        str1Ast = v2_arg0;
        y = v2_arg1;
        m = v1_arg0;
        str2Ast = v1_arg1;
    }

    const char *tmp = 0;
    m_strutil.is_string(str1Ast, &tmp);
    std::string str1Value(tmp);
    m_strutil.is_string(str2Ast, &tmp);
    std::string str2Value(tmp);

    int str1Len = str1Value.length();
    int str2Len = str2Value.length();

    //----------------------------------------
    //(a)  |---str1---|----y----|
    //     |--m--|-----str2-----|
    //
    //(b)  |---str1---|----y----|
    //     |-----m----|--str2---|
    //
    //(c)  |---str1---|----y----|
    //     |------m------|-str2-|
    //----------------------------------------

    std::list<int> overlapLen;
    overlapLen.push_back(0);

    for (int i = 1; i <= str1Len && i <= str2Len; i++) {
        if (str1Value.substr(str1Len - i, i) == str2Value.substr(0, i))
            overlapLen.push_back(i);
    }

    //----------------------------------------------------------------
    expr * commonVar = NULL;
    expr * xorFlag = NULL;
    std::pair<expr*, expr*> key1(concatAst1, concatAst2);
    std::pair<expr*, expr*> key2(concatAst2, concatAst1);

    // check the entries in this map to make sure they're still in scope
    // before we use them.

    std::map<std::pair<expr*,expr*>, std::map<int, expr*> >::iterator entry1 = varForBreakConcat.find(key1);
    std::map<std::pair<expr*,expr*>, std::map<int, expr*> >::iterator entry2 = varForBreakConcat.find(key2);

    bool entry1InScope;
    if (entry1 == varForBreakConcat.end()) {
        entry1InScope = false;
    } else {
        if (internal_variable_set.find((entry1->second)[0]) == internal_variable_set.end()
                /* || internal_variable_set.find((entry1->second)[1]) == internal_variable_set.end() */) {
            entry1InScope = false;
        } else {
            entry1InScope = true;
        }
    }

    bool entry2InScope;
    if (entry2 == varForBreakConcat.end()) {
        entry2InScope = false;
    } else {
        if (internal_variable_set.find((entry2->second)[0]) == internal_variable_set.end()
                /* || internal_variable_set.find((entry2->second)[1]) == internal_variable_set.end() */) {
            entry2InScope = false;
        } else {
            entry2InScope = true;
        }
    }

    TRACE("t_str_detail", tout << "entry 1 " << (entry1InScope ? "in scope" : "not in scope") << std::endl
            << "entry 2 " << (entry2InScope ? "in scope" : "not in scope") << std::endl;);

    if (!entry1InScope && !entry2InScope) {
        commonVar = mk_nonempty_str_var();
        xorFlag = mk_internal_xor_var();
        varForBreakConcat[key1][0] = commonVar;
        varForBreakConcat[key1][1] = xorFlag;
    } else {
        if (entry1InScope) {
            commonVar = (entry1->second)[0];
            xorFlag = (entry1->second)[1];
        } else {
            commonVar = (entry2->second)[0];
            xorFlag = (entry2->second)[1];
        }
        refresh_theory_var(commonVar);
    }

    expr ** or_item = alloc_svect(expr*, (overlapLen.size() + 1));
    int option = 0;
    expr ** and_item = alloc_svect(expr*, (1 + 4 * (overlapLen.size() + 1)));
    int pos = 1;

    if (!avoidLoopCut || !has_self_cut(m, y)) {
        or_item[option] = ctx.mk_eq_atom(xorFlag, mk_int(option));

        expr_ref str1_commonVar(mk_concat(str1Ast, commonVar), mgr);
        and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(m, str1_commonVar));

        expr_ref commonVar_str2(mk_concat(commonVar, str2Ast), mgr);
        and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(y, commonVar_str2));

        and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(mk_strlen(m),
                m_autil.mk_add(mk_strlen(str1Ast), mk_strlen(commonVar)) ));

        //    addItems[0] = mk_length(t, commonVar);
        //    addItems[1] = mk_length(t, str2Ast);
        //    and_item[pos++] = Z3_mk_eq(ctx, or_item[option], Z3_mk_eq(ctx, mk_length(t, y), Z3_mk_add(ctx, 2, addItems)));

        option++;
    } else {
        loopDetected = true;
        TRACE("t_str", tout << "AVOID LOOP: SKIPPED." << std::endl;);
        // TODO printCutVAR(m, y)
    }

    for (std::list<int>::iterator itor = overlapLen.begin(); itor != overlapLen.end(); itor++) {
        int overLen = *itor;
        std::string prefix = str1Value.substr(0, str1Len - overLen);
        std::string suffix = str2Value.substr(overLen, str2Len - overLen);
        or_item[option] = ctx.mk_eq_atom(xorFlag, mk_int(option));

        expr_ref prefixAst(m_strutil.mk_string(prefix), mgr);
        expr_ref x_eq_prefix(ctx.mk_eq_atom(m, prefixAst), mgr);
        and_item[pos++] = ctx.mk_eq_atom(or_item[option], x_eq_prefix);

        and_item[pos++] = ctx.mk_eq_atom(or_item[option],
                ctx.mk_eq_atom(mk_strlen(m), mk_strlen(prefixAst)));

        // adding length constraint for _ = constStr seems slowing things down.

        expr_ref suffixAst(m_strutil.mk_string(suffix), mgr);
        expr_ref y_eq_suffix(ctx.mk_eq_atom(y, suffixAst), mgr);
        and_item[pos++] = ctx.mk_eq_atom(or_item[option], y_eq_suffix);

        and_item[pos++] = ctx.mk_eq_atom(or_item[option], ctx.mk_eq_atom(mk_strlen(y), mk_strlen(suffixAst)));

        option++;
    }

    //  case 6: concat("str1", y) = concat(m, "str2")
    and_item[0] = mgr.mk_or(option, or_item);
    expr_ref implyR(mgr.mk_and(pos, and_item), mgr);
    assert_implication(ctx.mk_eq_atom(concatAst1, concatAst2), implyR);
}

void theory_str::process_unroll_eq_const_str(expr * unrollFunc, expr * constStr) {
	ast_manager & m = get_manager();

	if (!is_Unroll(to_app(unrollFunc))) {
		return;
	}
	if (!m_strutil.is_string(constStr)) {
		return;
	}

	expr * funcInUnroll = to_app(unrollFunc)->get_arg(0);
	std::string strValue = m_strutil.get_string_constant_value(constStr);

	TRACE("t_str_detail", tout << "unrollFunc: " << mk_pp(unrollFunc, m) << std::endl
			<< "constStr: " << mk_pp(constStr, m) << std::endl;);

	if (strValue == "") {
		return;
	}

	if (is_Str2Reg(to_app(funcInUnroll))) {
		unroll_str2reg_constStr(unrollFunc, constStr);
		return;
	}
}

void theory_str::process_concat_eq_unroll(expr * concat, expr * unroll) {
	context & ctx = get_context();
	ast_manager & mgr = get_manager();

	TRACE("t_str_detail", tout << "concat = " << mk_pp(concat, mgr) << ", unroll = " << mk_pp(unroll, mgr) << std::endl;);

	std::pair<expr*, expr*> key = std::make_pair(concat, unroll);
	expr_ref toAssert(mgr);

	if (concat_eq_unroll_ast_map.find(key) == concat_eq_unroll_ast_map.end()) {
		expr_ref arg1(to_app(concat)->get_arg(0), mgr);
		expr_ref arg2(to_app(concat)->get_arg(1), mgr);
		expr_ref r1(to_app(unroll)->get_arg(0), mgr);
		expr_ref t1(to_app(unroll)->get_arg(1), mgr);

		expr_ref v1(mk_regex_rep_var(), mgr);
		expr_ref v2(mk_regex_rep_var(), mgr);
		expr_ref v3(mk_regex_rep_var(), mgr);
		expr_ref v4(mk_regex_rep_var(), mgr);
		expr_ref v5(mk_regex_rep_var(), mgr);

		expr_ref t2(mk_unroll_bound_var(), mgr);
		expr_ref t3(mk_unroll_bound_var(), mgr);
		expr_ref emptyStr(m_strutil.mk_string(""), mgr);

		expr_ref unroll1(mk_unroll(r1, t2), mgr);
		expr_ref unroll2(mk_unroll(r1, t3), mgr);

		expr_ref op0(ctx.mk_eq_atom(t1, mk_int(0)), mgr);
		expr_ref op1(m_autil.mk_ge(t1, mk_int(1)), mgr);

		expr_ref_vector op1Items(mgr);
		expr_ref_vector op2Items(mgr);

		op1Items.push_back(ctx.mk_eq_atom(arg1, emptyStr));
		op1Items.push_back(ctx.mk_eq_atom(arg2, emptyStr));
		op1Items.push_back(ctx.mk_eq_atom(mk_strlen(arg1), mk_int(0)));
		op1Items.push_back(ctx.mk_eq_atom(mk_strlen(arg2), mk_int(0)));
		expr_ref opAnd1(ctx.mk_eq_atom(op0, mk_and(op1Items)), mgr);

		expr_ref v1v2(mk_concat(v1, v2), mgr);
		op2Items.push_back(ctx.mk_eq_atom(arg1, v1v2));
		op2Items.push_back(ctx.mk_eq_atom(mk_strlen(arg1), m_autil.mk_add(mk_strlen(v1), mk_strlen(v2))));
		expr_ref v3v4(mk_concat(v3, v4), mgr);
		op2Items.push_back(ctx.mk_eq_atom(arg2, v3v4));
		op2Items.push_back(ctx.mk_eq_atom(mk_strlen(arg2), m_autil.mk_add(mk_strlen(v3), mk_strlen(v4))));

		op2Items.push_back(ctx.mk_eq_atom(v1, unroll1));
		op2Items.push_back(ctx.mk_eq_atom(mk_strlen(v1), mk_strlen(unroll1)));
		op2Items.push_back(ctx.mk_eq_atom(v4, unroll2));
		op2Items.push_back(ctx.mk_eq_atom(mk_strlen(v4), mk_strlen(unroll2)));
		expr_ref v2v3(mk_concat(v2, v3), mgr);
		op2Items.push_back(ctx.mk_eq_atom(v5, v2v3));
		reduce_virtual_regex_in(v5, r1, op2Items);
		op2Items.push_back(ctx.mk_eq_atom(mk_strlen(v5), m_autil.mk_add(mk_strlen(v2), mk_strlen(v3))));
		op2Items.push_back(ctx.mk_eq_atom(m_autil.mk_add(t2, t3), m_autil.mk_add(t1, mk_int(-1))));
		expr_ref opAnd2(ctx.mk_eq_atom(op1, mk_and(op2Items)), mgr);

		toAssert = mgr.mk_and(opAnd1, opAnd2);
		m_trail.push_back(toAssert);
		concat_eq_unroll_ast_map[key] = toAssert;
	} else {
		toAssert = concat_eq_unroll_ast_map[key];
	}

	assert_axiom(toAssert);
}

void theory_str::unroll_str2reg_constStr(expr * unrollFunc, expr * eqConstStr) {
	context & ctx = get_context();
	ast_manager & m = get_manager();

	expr * str2RegFunc = to_app(unrollFunc)->get_arg(0);
	expr * strInStr2RegFunc = to_app(str2RegFunc)->get_arg(0);
	expr * oriCnt = to_app(unrollFunc)->get_arg(1);

	std::string strValue = m_strutil.get_string_constant_value(eqConstStr);
	std::string regStrValue = m_strutil.get_string_constant_value(strInStr2RegFunc);
	int strLen = strValue.length();
	int regStrLen = regStrValue.length();
	int cnt = strLen / regStrLen; // TODO prevent DIV/0 on regStrLen

	expr_ref implyL(ctx.mk_eq_atom(unrollFunc, eqConstStr), m);
	expr_ref implyR1(ctx.mk_eq_atom(oriCnt, mk_int(cnt)), m);
	expr_ref implyR2(ctx.mk_eq_atom(mk_strlen(unrollFunc), mk_int(strLen)), m);
	expr_ref axiomRHS(m.mk_and(implyR1, implyR2), m);
	SASSERT(implyL);
	SASSERT(axiomRHS);
	assert_implication(implyL, axiomRHS);
}

/*
 * Look through the equivalence class of n to find a string constant.
 * Return that constant if it is found, and set hasEqcValue to true.
 * Otherwise, return n, and set hasEqcValue to false.
 */
/*
expr * theory_str::get_eqc_value(expr * n, bool & hasEqcValue) {
	context & ctx = get_context();
	// I hope this works
	ctx.internalize(n, false);
	enode * nNode = ctx.get_enode(n);
	enode * eqcNode = nNode;
	do {
		app * ast = eqcNode->get_owner();
		if (is_string(eqcNode)) {
			hasEqcValue = true;
			return ast;
		}
		eqcNode = eqcNode->get_next();
	} while (eqcNode != nNode);
	// not found
	hasEqcValue = false;
	return n;
}
*/

expr * theory_str::get_eqc_value(expr * n, bool & hasEqcValue) {
    return z3str2_get_eqc_value(n, hasEqcValue);
}


// Simulate the behaviour of get_eqc_value() from Z3str2.
// We only check m_find for a string constant.

expr * theory_str::z3str2_get_eqc_value(expr * n , bool & hasEqcValue) {
    expr * curr = n;
    do {
        if (m_strutil.is_string(curr)) {
            hasEqcValue = true;
            return curr;
        }
        curr = get_eqc_next(curr);
    } while (curr != n);
    hasEqcValue = false;
    return n;
}

// from Z3: theory_seq.cpp

static theory_mi_arith* get_th_arith(context& ctx, theory_id afid, expr* e) {
    theory* th = ctx.get_theory(afid);
    if (th && ctx.e_internalized(e)) {
        return dynamic_cast<theory_mi_arith*>(th);
    }
    else {
        return 0;
    }
}

bool theory_str::get_value(expr* e, rational& val) const {
    if (opt_DisableIntegerTheoryIntegration) {
        TRACE("t_str_detail", tout << "WARNING: integer theory integration disabled" << std::endl;);
        return false;
    }

    context& ctx = get_context();
    ast_manager & m = get_manager();
    theory_mi_arith* tha = get_th_arith(ctx, m_autil.get_family_id(), e);
    if (!tha) {
        return false;
    }
    TRACE("t_str_int", tout << "checking eqc of " << mk_pp(e, m) << " for arithmetic value" << std::endl;);
    expr_ref _val(m);
    enode * en_e = ctx.get_enode(e);
    enode * it = en_e;
    do {
        if (m_autil.is_numeral(it->get_owner(), val) && val.is_int()) {
            // found an arithmetic term
            TRACE("t_str_int", tout << mk_pp(it->get_owner(), m) << " is an integer ( ~= " << val << " )"
                    << std::endl;);
            return true;
        } else {
            TRACE("t_str_int", tout << mk_pp(it->get_owner(), m) << " not a numeral" << std::endl;);
        }
        it = it->get_next();
    } while (it != en_e);
    TRACE("t_str_int", tout << "no arithmetic values found in eqc" << std::endl;);
    return false;
}

bool theory_str::lower_bound(expr* _e, rational& lo) {
    if (opt_DisableIntegerTheoryIntegration) {
        TRACE("t_str_detail", tout << "WARNING: integer theory integration disabled" << std::endl;);
        return false;
    }

    context& ctx = get_context();
    ast_manager & m = get_manager();
    theory_mi_arith* tha = get_th_arith(ctx, m_autil.get_family_id(), _e);
    expr_ref _lo(m);
    if (!tha || !tha->get_lower(ctx.get_enode(_e), _lo)) return false;
    return m_autil.is_numeral(_lo, lo) && lo.is_int();
}

bool theory_str::upper_bound(expr* _e, rational& hi) {
    if (opt_DisableIntegerTheoryIntegration) {
        TRACE("t_str_detail", tout << "WARNING: integer theory integration disabled" << std::endl;);
        return false;
    }

    context& ctx = get_context();
    ast_manager & m = get_manager();
    theory_mi_arith* tha = get_th_arith(ctx, m_autil.get_family_id(), _e);
    expr_ref _hi(m);
    if (!tha || !tha->get_upper(ctx.get_enode(_e), _hi)) return false;
    return m_autil.is_numeral(_hi, hi) && hi.is_int();
}

bool theory_str::get_len_value(expr* e, rational& val) {
    if (opt_DisableIntegerTheoryIntegration) {
        TRACE("t_str_detail", tout << "WARNING: integer theory integration disabled" << std::endl;);
        return false;
    }

    context& ctx = get_context();
    ast_manager & m = get_manager();

    theory* th = ctx.get_theory(m_autil.get_family_id());
    if (!th) {
        TRACE("t_str_int", tout << "oops, can't get m_autil's theory" << std::endl;);
        return false;
    }
    theory_mi_arith* tha = dynamic_cast<theory_mi_arith*>(th);
    if (!tha) {
        TRACE("t_str_int", tout << "oops, can't cast to theory_mi_arith" << std::endl;);
        return false;
    }

    TRACE("t_str_int", tout << "checking len value of " << mk_ismt2_pp(e, m) << std::endl;);

    rational val1;
    expr_ref len(m), len_val(m);
    expr* e1, *e2;
    ptr_vector<expr> todo;
    todo.push_back(e);
    val.reset();
    while (!todo.empty()) {
        expr* c = todo.back();
        todo.pop_back();
        if (is_concat(to_app(c))) {
            e1 = to_app(c)->get_arg(0);
            e2 = to_app(c)->get_arg(1);
            todo.push_back(e1);
            todo.push_back(e2);
        }
        else if (is_string(to_app(c))) {
            int sl = m_strutil.get_string_constant_value(c).length();
            val += rational(sl);
        }
        else {
            len = mk_strlen(c);

            // debugging
            TRACE("t_str_int", {
               tout << mk_pp(len, m) << ":" << std::endl
               << (ctx.is_relevant(len.get()) ? "relevant" : "not relevant") << std::endl
               << (ctx.e_internalized(len) ? "internalized" : "not internalized") << std::endl
               ;
               if (ctx.e_internalized(len)) {
                   enode * e_len = ctx.get_enode(len);
                   tout << "has " << e_len->get_num_th_vars() << " theory vars" << std::endl;

                   // eqc debugging
                   {
                       tout << "dump equivalence class of " << mk_pp(len, get_manager()) << std::endl;
                       enode * nNode = ctx.get_enode(len);
                       enode * eqcNode = nNode;
                       do {
                           app * ast = eqcNode->get_owner();
                           tout << mk_pp(ast, get_manager()) << std::endl;
                           eqcNode = eqcNode->get_next();
                       } while (eqcNode != nNode);
                   }
               }
            });

            if (ctx.e_internalized(len) && get_value(len, val1)) {
                val += val1;
                TRACE("t_str_int", tout << "integer theory: subexpression " << mk_ismt2_pp(len, m) << " has length " << val1 << std::endl;);
            }
            else {
                TRACE("t_str_int", tout << "integer theory: subexpression " << mk_ismt2_pp(len, m) << " has no length assignment; bailing out" << std::endl;);
                return false;
            }
        }
    }

    TRACE("t_str_int", tout << "length of " << mk_ismt2_pp(e, m) << " is " << val << std::endl;);
    return val.is_int();
}

/*
 * Decide whether n1 and n2 are already in the same equivalence class.
 * This only checks whether the core considers them to be equal;
 * they may not actually be equal.
 */
bool theory_str::in_same_eqc(expr * n1, expr * n2) {
    if (n1 == n2) return true;
    context & ctx = get_context();
    ast_manager & m = get_manager();

    // similar to get_eqc_value(), make absolutely sure
    // that we've set this up properly for the context

    if (!ctx.e_internalized(n1)) {
        TRACE("t_str_detail", tout << "WARNING: expression " << mk_ismt2_pp(n1, m) << " was not internalized" << std::endl;);
        ctx.internalize(n1, false);
    }
    if (!ctx.e_internalized(n2)) {
        TRACE("t_str_detail", tout << "WARNING: expression " << mk_ismt2_pp(n2, m) << " was not internalized" << std::endl;);
        ctx.internalize(n2, false);
    }

    expr * curr = get_eqc_next(n1);
    while (curr != n1) {
        if (curr == n2)
            return true;
        curr = get_eqc_next(curr);
    }
    return false;
}

expr * theory_str::collect_eq_nodes(expr * n, expr_ref_vector & eqcSet) {
    context & ctx = get_context();
    expr * constStrNode = NULL;

    expr * ex = n;
    do {
        if (m_strutil.is_string(to_app(ex))) {
            constStrNode = ex;
        }
        eqcSet.push_back(ex);

        ex = get_eqc_next(ex);
    } while (ex != n);
    return constStrNode;
}

/*
 * Collect constant strings (from left to right) in an AST node.
 */
void theory_str::get_const_str_asts_in_node(expr * node, expr_ref_vector & astList) {
    ast_manager & m = get_manager();
    if (m_strutil.is_string(node)) {
        astList.push_back(node);
    //} else if (getNodeType(t, node) == my_Z3_Func) {
    } else if (is_app(node)) {
        app * func_app = to_app(node);
        unsigned int argCount = func_app->get_num_args();
        for (unsigned int i = 0; i < argCount; i++) {
            expr * argAst = func_app->get_arg(i);
            get_const_str_asts_in_node(argAst, astList);
        }
    }
}

void theory_str::check_contain_by_eqc_val(expr * varNode, expr * constNode) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    TRACE("t_str_detail", tout << "varNode = " << mk_pp(varNode, m) << ", constNode = " << mk_pp(constNode, m) << std::endl;);

    expr_ref_vector litems(m);

    // TODO refactor to use the new contain_pair_idx_map

    expr_ref_vector::iterator itor1 = contains_map.begin();
    for (; itor1 != contains_map.end(); ++itor1) {
        expr * boolVar = *itor1;
        // boolVar is actually a Contains term
        app * containsApp = to_app(boolVar);
        expr * strAst = containsApp->get_arg(0);
        expr * substrAst = containsApp->get_arg(1);

        // we only want to inspect the Contains terms where either of strAst or substrAst
        // are equal to varNode.

        TRACE("t_str_detail", tout << "considering Contains with strAst = " << mk_pp(strAst, m) << ", substrAst = " << mk_pp(substrAst, m) << "..." << std::endl;);

        if (varNode != strAst && varNode != substrAst) {
            TRACE("t_str_detail", tout << "varNode not equal to strAst or substrAst, skip" << std::endl;);
            continue;
        }
        TRACE("t_str_detail", tout << "varNode matched one of strAst or substrAst. Continuing" << std::endl;);

        // varEqcNode is str
        if (strAst == varNode) {
            expr_ref implyR(m);
            litems.reset();

            if (strAst != constNode) {
                litems.push_back(ctx.mk_eq_atom(strAst, constNode));
            }
            std::string strConst = m_strutil.get_string_constant_value(constNode);
            bool subStrHasEqcValue = false;
            expr * substrValue = get_eqc_value(substrAst, subStrHasEqcValue);
            if (substrValue != substrAst) {
                litems.push_back(ctx.mk_eq_atom(substrAst, substrValue));
            }

            if (subStrHasEqcValue) {
                // subStr has an eqc constant value
                std::string subStrConst = m_strutil.get_string_constant_value(substrValue);

                TRACE("t_str_detail", tout << "strConst = " << strConst << ", subStrConst = " << subStrConst << std::endl;);

                if (strConst.find(subStrConst) != std::string::npos) {
                    //implyR = ctx.mk_eq(ctx, boolVar, Z3_mk_true(ctx));
                    implyR = boolVar;
                } else {
                    //implyR = Z3_mk_eq(ctx, boolVar, Z3_mk_false(ctx));
                    implyR = m.mk_not(boolVar);
                }
            } else {
                // ------------------------------------------------------------------------------------------------
                // subStr doesn't have an eqc contant value
                // however, subStr equals to some concat(arg_1, arg_2, ..., arg_n)
                // if arg_j is a constant and is not a part of the strConst, it's sure that the contains is false
                // ** This check is needed here because the "strConst" and "strAst" may not be in a same eqc yet
                // ------------------------------------------------------------------------------------------------
                // collect eqc concat
                std::set<expr*> eqcConcats;
                get_concats_in_eqc(substrAst, eqcConcats);
                for (std::set<expr*>::iterator concatItor = eqcConcats.begin();
                        concatItor != eqcConcats.end(); concatItor++) {
                    expr_ref_vector constList(m);
                    bool counterEgFound = false;
                    // get constant strings in concat
                    expr * aConcat = *concatItor;
                    get_const_str_asts_in_node(aConcat, constList);
                    for (expr_ref_vector::iterator cstItor = constList.begin();
                            cstItor != constList.end(); cstItor++) {
                        std::string pieceStr = m_strutil.get_string_constant_value(*cstItor);
                        if (strConst.find(pieceStr) == std::string::npos) {
                            counterEgFound = true;
                            if (aConcat != substrAst) {
                                litems.push_back(ctx.mk_eq_atom(substrAst, aConcat));
                            }
                            //implyR = Z3_mk_eq(ctx, boolVar, Z3_mk_false(ctx));
                            implyR = m.mk_not(boolVar);
                            break;
                        }
                    }
                    if (counterEgFound) {
                        TRACE("t_str_detail", tout << "Inconsistency found!" << std::endl;);
                        break;
                    }
                }
            }
            // add assertion
            if (implyR) {
                expr_ref implyLHS(mk_and(litems), m);
                assert_implication(implyLHS, implyR);
            }
        }
        // varEqcNode is subStr
        else if (substrAst == varNode) {
            expr_ref implyR(m);
            litems.reset();

            if (substrAst != constNode) {
                litems.push_back(ctx.mk_eq_atom(substrAst, constNode));
            }
            bool strHasEqcValue = false;
            expr * strValue = get_eqc_value(strAst, strHasEqcValue);
            if (strValue != strAst) {
                litems.push_back(ctx.mk_eq_atom(strAst, strValue));
            }

            if (strHasEqcValue) {
                std::string strConst = m_strutil.get_string_constant_value(strValue);
                std::string subStrConst = m_strutil.get_string_constant_value(constNode);
                if (strConst.find(subStrConst) != std::string::npos) {
                    //implyR = Z3_mk_eq(ctx, boolVar, Z3_mk_true(ctx));
                    implyR = boolVar;
                } else {
                    // implyR = Z3_mk_eq(ctx, boolVar, Z3_mk_false(ctx));
                    implyR = m.mk_not(boolVar);
                }
            }

            // add assertion
            if (implyR) {
                expr_ref implyLHS(mk_and(litems), m);
                assert_implication(implyLHS, implyR);
            }
        }
    } // for (itor1 : contains_map)
}

void theory_str::check_contain_by_substr(expr * varNode, expr_ref_vector & willEqClass) {
    context & ctx = get_context();
    ast_manager & m = get_manager();
    expr_ref_vector litems(m);

    // TODO refactor to use the new contain_pair_idx_map

    expr_ref_vector::iterator itor1 = contains_map.begin();
    for (; itor1 != contains_map.end(); ++itor1) {
        expr * boolVar = *itor1;
        // boolVar is actually a Contains term
        app * containsApp = to_app(boolVar);
        expr * strAst = containsApp->get_arg(0);
        expr * substrAst = containsApp->get_arg(1);

        // we only want to inspect the Contains terms where either of strAst or substrAst
        // are equal to varNode.

        TRACE("t_str_detail", tout << "considering Contains with strAst = " << mk_pp(strAst, m) << ", substrAst = " << mk_pp(substrAst, m) << "..." << std::endl;);

        if (varNode != strAst && varNode != substrAst) {
            TRACE("t_str_detail", tout << "varNode not equal to strAst or substrAst, skip" << std::endl;);
            continue;
        }
        TRACE("t_str_detail", tout << "varNode matched one of strAst or substrAst. Continuing" << std::endl;);

        if (substrAst == varNode) {
            bool strAstHasVal = false;
            expr * strValue = get_eqc_value(strAst, strAstHasVal);
            if (strAstHasVal) {
                TRACE("t_str_detail", tout << mk_pp(strAst, m) << " has constant eqc value " << mk_pp(strValue, m) << std::endl;);
                if (strValue != strAst) {
                    litems.push_back(ctx.mk_eq_atom(strAst, strValue));
                }
                std::string strConst = m_strutil.get_string_constant_value(strValue);
                // iterate eqc (also eqc-to-be) of substr
                for (expr_ref_vector::iterator itAst = willEqClass.begin(); itAst != willEqClass.end(); itAst++) {
                    bool counterEgFound = false;
                    if (is_concat(to_app(*itAst))) {
                        expr_ref_vector constList(m);
                        // get constant strings in concat
                        app * aConcat = to_app(*itAst);
                        get_const_str_asts_in_node(aConcat, constList);
                        for (expr_ref_vector::iterator cstItor = constList.begin();
                                cstItor != constList.end(); cstItor++) {
                            std::string pieceStr = m_strutil.get_string_constant_value(*cstItor);
                            if (strConst.find(pieceStr) == std::string::npos) {
                                TRACE("t_str_detail", tout << "Inconsistency found!" << std::endl;);
                                counterEgFound = true;
                                if (aConcat != substrAst) {
                                    litems.push_back(ctx.mk_eq_atom(substrAst, aConcat));
                                }
                                expr_ref implyLHS(mk_and(litems), m);
                                expr_ref implyR(m.mk_not(boolVar), m);
                                assert_implication(implyLHS, implyR);
                                break;
                            }
                        }
                    }
                    if (counterEgFound) {
                        break;
                    }
                }
            }
        }
    }
}

bool theory_str::in_contain_idx_map(expr * n) {
    return contain_pair_idx_map.find(n) != contain_pair_idx_map.end();
}

void theory_str::check_contain_by_eq_nodes(expr * n1, expr * n2) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    if (in_contain_idx_map(n1) && in_contain_idx_map(n2)) {
        std::set<std::pair<expr*, expr*> >::iterator keysItor1 = contain_pair_idx_map[n1].begin();
        std::set<std::pair<expr*, expr*> >::iterator keysItor2;

        for (; keysItor1 != contain_pair_idx_map[n1].end(); keysItor1++) {
            // keysItor1 is on set {<.., n1>, ..., <n1, ...>, ...}
            std::pair<expr*, expr*> key1 = *keysItor1;
            if (key1.first == n1 && key1.second == n2) {
                expr_ref implyL(m);
                expr_ref implyR(contain_pair_bool_map[key1], m);
                if (n1 != n2) {
                    implyL = ctx.mk_eq_atom(n1, n2);
                    assert_implication(implyL, implyR);
                } else {
                    assert_axiom(implyR);
                }
            }

            for (keysItor2 = contain_pair_idx_map[n2].begin();
                    keysItor2 != contain_pair_idx_map[n2].end(); keysItor2++) {
                // keysItor2 is on set {<.., n2>, ..., <n2, ...>, ...}
                std::pair<expr*, expr*> key2 = *keysItor2;
                // skip if the pair is eq
                if (key1 == key2) {
                    continue;
                }

                // ***************************
                // Case 1: Contains(m, ...) /\ Contains(n, ) /\ m = n
                // ***************************
                if (key1.first == n1 && key2.first == n2) {
                    expr * subAst1 = key1.second;
                    expr * subAst2 = key2.second;
                    bool subAst1HasValue = false;
                    bool subAst2HasValue = false;
                    expr * subValue1 = get_eqc_value(subAst1, subAst1HasValue);
                    expr * subValue2 = get_eqc_value(subAst2, subAst2HasValue);

                    TRACE("t_str_detail",
                            tout << "(Contains " << mk_pp(n1, m) << " " << mk_pp(subAst1, m) << ")" << std::endl;
                            tout << "(Contains " << mk_pp(n2, m) << " " << mk_pp(subAst2, m) << ")" << std::endl;
                            if (subAst1 != subValue1) {
                                tout << mk_pp(subAst1, m) << " = " << mk_pp(subValue1, m) << std::endl;
                            }
                            if (subAst2 != subValue2) {
                                tout << mk_pp(subAst2, m) << " = " << mk_pp(subValue2, m) << std::endl;
                            }
                            );

                    if (subAst1HasValue && subAst2HasValue) {
                        expr_ref_vector litems1(m);
                        if (n1 != n2) {
                            litems1.push_back(ctx.mk_eq_atom(n1, n2));
                        }
                        if (subValue1 != subAst1) {
                            litems1.push_back(ctx.mk_eq_atom(subAst1, subValue1));
                        }
                        if (subValue2 != subAst2) {
                            litems1.push_back(ctx.mk_eq_atom(subAst2, subValue2));
                        }

                        std::string subConst1 = m_strutil.get_string_constant_value(subValue1);
                        std::string subConst2 = m_strutil.get_string_constant_value(subValue2);
                        expr_ref implyR(m);
                        if (subConst1 == subConst2) {
                            // key1.first = key2.first /\ key1.second = key2.second
                            // ==> (containPairBoolMap[key1] = containPairBoolMap[key2])
                            implyR = ctx.mk_eq_atom(contain_pair_bool_map[key1], contain_pair_bool_map[key2]);
                        } else if (subConst1.find(subConst2) != std::string::npos) {
                            // key1.first = key2.first /\ Contains(key1.second, key2.second)
                            // ==> (containPairBoolMap[key1] --> containPairBoolMap[key2])
                            implyR = rewrite_implication(contain_pair_bool_map[key1], contain_pair_bool_map[key2]);
                        } else if (subConst2.find(subConst1) != std::string::npos) {
                            // key1.first = key2.first /\ Contains(key2.second, key1.second)
                            // ==> (containPairBoolMap[key2] --> containPairBoolMap[key1])
                            implyR = rewrite_implication(contain_pair_bool_map[key2], contain_pair_bool_map[key1]);
                        }

                        if (implyR) {
                            if (litems1.empty()) {
                                assert_axiom(implyR);
                            } else {
                                assert_implication(mk_and(litems1), implyR);
                            }
                        }
                    } else {
                        expr_ref_vector subAst1Eqc(m);
                        expr_ref_vector subAst2Eqc(m);
                        collect_eq_nodes(subAst1, subAst1Eqc);
                        collect_eq_nodes(subAst2, subAst2Eqc);

                        if (subAst1Eqc.contains(subAst2)) {
                            // -----------------------------------------------------------
                            // * key1.first = key2.first /\ key1.second = key2.second
                            //   -->  containPairBoolMap[key1] = containPairBoolMap[key2]
                            // -----------------------------------------------------------
                            expr_ref_vector litems2(m);
                            if (n1 != n2) {
                                litems2.push_back(ctx.mk_eq_atom(n1, n2));
                            }
                            if (subAst1 != subAst2) {
                                litems2.push_back(ctx.mk_eq_atom(subAst1, subAst2));
                            }
                            expr_ref implyR(ctx.mk_eq_atom(contain_pair_bool_map[key1], contain_pair_bool_map[key2]), m);
                            if (litems2.empty()) {
                                assert_axiom(implyR);
                            } else {
                                assert_implication(mk_and(litems2), implyR);
                            }
                        } else {
                            // -----------------------------------------------------------
                            // * key1.first = key2.first
                            //   check eqc(key1.second) and eqc(key2.second)
                            // -----------------------------------------------------------
                            expr_ref_vector::iterator eqItorSub1 = subAst1Eqc.begin();
                            for (; eqItorSub1 != subAst1Eqc.end(); eqItorSub1++) {
                                expr_ref_vector::iterator eqItorSub2 = subAst2Eqc.begin();
                                for (; eqItorSub2 != subAst2Eqc.end(); eqItorSub2++) {
                                    // ------------
                                    // key1.first = key2.first /\ containPairBoolMap[<eqc(key1.second), eqc(key2.second)>]
                                    // ==>  (containPairBoolMap[key1] --> containPairBoolMap[key2])
                                    // ------------
                                    {
                                        expr_ref_vector litems3(m);
                                        if (n1 != n2) {
                                            litems3.push_back(ctx.mk_eq_atom(n1, n2));
                                        }
                                        expr * eqSubVar1 = *eqItorSub1;
                                        if (eqSubVar1 != subAst1) {
                                            litems3.push_back(ctx.mk_eq_atom(subAst1, eqSubVar1));
                                        }
                                        expr * eqSubVar2 = *eqItorSub2;
                                        if (eqSubVar2 != subAst2) {
                                            litems3.push_back(ctx.mk_eq_atom(subAst2, eqSubVar2));
                                        }
                                        std::pair<expr*, expr*> tryKey1 = std::make_pair(eqSubVar1, eqSubVar2);
                                        if (contain_pair_bool_map.contains(tryKey1)) {
                                            TRACE("t_str_detail", tout << "(Contains " << mk_pp(eqSubVar1, m) << " " << mk_pp(eqSubVar2, m) << ")" << std::endl;);
                                            litems3.push_back(contain_pair_bool_map[tryKey1]);
                                            expr_ref implR(rewrite_implication(contain_pair_bool_map[key1], contain_pair_bool_map[key2]), m);
                                            assert_implication(mk_and(litems3), implR);
                                        }
                                    }
                                    // ------------
                                    // key1.first = key2.first /\ containPairBoolMap[<eqc(key2.second), eqc(key1.second)>]
                                    // ==>  (containPairBoolMap[key2] --> containPairBoolMap[key1])
                                    // ------------
                                    {
                                        expr_ref_vector litems4(m);
                                        if (n1 != n2) {
                                            litems4.push_back(ctx.mk_eq_atom(n1, n2));
                                        }
                                        expr * eqSubVar1 = *eqItorSub1;
                                        if (eqSubVar1 != subAst1) {
                                            litems4.push_back(ctx.mk_eq_atom(subAst1, eqSubVar1));
                                        }
                                        expr * eqSubVar2 = *eqItorSub2;
                                        if (eqSubVar2 != subAst2) {
                                            litems4.push_back(ctx.mk_eq_atom(subAst2, eqSubVar2));
                                        }
                                        std::pair<expr*, expr*> tryKey2 = std::make_pair(eqSubVar2, eqSubVar1);
                                        if (contain_pair_bool_map.contains(tryKey2)) {
                                            TRACE("t_str_detail", tout << "(Contains " << mk_pp(eqSubVar2, m) << " " << mk_pp(eqSubVar1, m) << ")" << std::endl;);
                                            litems4.push_back(contain_pair_bool_map[tryKey2]);
                                            expr_ref implR(rewrite_implication(contain_pair_bool_map[key2], contain_pair_bool_map[key1]), m);
                                            assert_implication(mk_and(litems4), implR);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                // ***************************
                // Case 2: Contains(..., m) /\ Contains(... , n) /\ m = n
                // ***************************
                else if (key1.second == n1 && key2.second == n2) {
                    expr * str1 = key1.first;
                    expr * str2 = key2.first;
                    bool str1HasValue = false;
                    bool str2HasValue = false;
                    expr * strVal1 = get_eqc_value(str1, str1HasValue);
                    expr * strVal2 = get_eqc_value(str2, str2HasValue);

                    TRACE("t_str_detail",
                            tout << "(Contains " << mk_pp(str1, m) << " " << mk_pp(n1, m) << ")" << std::endl;
                            tout << "(Contains " << mk_pp(str2, m) << " " << mk_pp(n2, m) << ")" << std::endl;
                            if (str1 != strVal1) {
                                tout << mk_pp(str1, m) << " = " << mk_pp(strVal1, m) << std::endl;
                            }
                            if (str2 != strVal2) {
                                tout << mk_pp(str2, m) << " = " << mk_pp(strVal2, m) << std::endl;
                            }
                            );

                    if (str1HasValue && str2HasValue) {
                        expr_ref_vector litems1(m);
                        if (n1 != n2) {
                            litems1.push_back(ctx.mk_eq_atom(n1, n2));
                        }
                        if (strVal1 != str1) {
                            litems1.push_back(ctx.mk_eq_atom(str1, strVal1));
                        }
                        if (strVal2 != str2) {
                            litems1.push_back(ctx.mk_eq_atom(str2, strVal2));
                        }

                        std::string const1 = m_strutil.get_string_constant_value(strVal1);
                        std::string const2 = m_strutil.get_string_constant_value(strVal2);
                        expr_ref implyR(m);

                        if (const1 == const2) {
                            // key1.second = key2.second /\ key1.first = key2.first
                            // ==> (containPairBoolMap[key1] = containPairBoolMap[key2])
                            implyR = ctx.mk_eq_atom(contain_pair_bool_map[key1], contain_pair_bool_map[key2]);
                        } else if (const1.find(const2) != std::string::npos) {
                            // key1.second = key2.second /\ Contains(key1.first, key2.first)
                            // ==> (containPairBoolMap[key2] --> containPairBoolMap[key1])
                            implyR = rewrite_implication(contain_pair_bool_map[key2], contain_pair_bool_map[key1]);
                        } else if (const2.find(const1) != std::string::npos) {
                            // key1.first = key2.first /\ Contains(key2.first, key1.first)
                            // ==> (containPairBoolMap[key1] --> containPairBoolMap[key2])
                            implyR = rewrite_implication(contain_pair_bool_map[key1], contain_pair_bool_map[key2]);
                        }

                        if (implyR) {
                            if (litems1.size() == 0) {
                                assert_axiom(implyR);
                            } else {
                                assert_implication(mk_and(litems1), implyR);
                            }
                        }
                    }

                    else {
                        expr_ref_vector str1Eqc(m);
                        expr_ref_vector str2Eqc(m);
                        collect_eq_nodes(str1, str1Eqc);
                        collect_eq_nodes(str2, str2Eqc);

                        if (str1Eqc.contains(str2)) {
                            // -----------------------------------------------------------
                            // * key1.first = key2.first /\ key1.second = key2.second
                            //   -->  containPairBoolMap[key1] = containPairBoolMap[key2]
                            // -----------------------------------------------------------
                            expr_ref_vector litems2(m);
                            if (n1 != n2) {
                                litems2.push_back(ctx.mk_eq_atom(n1, n2));
                            }
                            if (str1 != str2) {
                                litems2.push_back(ctx.mk_eq_atom(str1, str2));
                            }
                            expr_ref implyR(ctx.mk_eq_atom(contain_pair_bool_map[key1], contain_pair_bool_map[key2]), m);
                            if (litems2.empty()) {
                                assert_axiom(implyR);
                            } else {
                                assert_implication(mk_and(litems2), implyR);
                            }
                        } else {
                            // -----------------------------------------------------------
                            // * key1.second = key2.second
                            //   check eqc(key1.first) and eqc(key2.first)
                            // -----------------------------------------------------------
                            expr_ref_vector::iterator eqItorStr1 = str1Eqc.begin();
                            for (; eqItorStr1 != str1Eqc.end(); eqItorStr1++) {
                                expr_ref_vector::iterator eqItorStr2 = str2Eqc.begin();
                                for (; eqItorStr2 != str2Eqc.end(); eqItorStr2++) {
                                    {
                                        expr_ref_vector litems3(m);
                                        if (n1 != n2) {
                                            litems3.push_back(ctx.mk_eq_atom(n1, n2));
                                        }
                                        expr * eqStrVar1 = *eqItorStr1;
                                        if (eqStrVar1 != str1) {
                                            litems3.push_back(ctx.mk_eq_atom(str1, eqStrVar1));
                                        }
                                        expr * eqStrVar2 = *eqItorStr2;
                                        if (eqStrVar2 != str2) {
                                            litems3.push_back(ctx.mk_eq_atom(str2, eqStrVar2));
                                        }
                                        std::pair<expr*, expr*> tryKey1 = std::make_pair(eqStrVar1, eqStrVar2);
                                        if (contain_pair_bool_map.contains(tryKey1)) {
                                            TRACE("t_str_detail", tout << "(Contains " << mk_pp(eqStrVar1, m) << " " << mk_pp(eqStrVar2, m) << ")" << std::endl;);
                                            litems3.push_back(contain_pair_bool_map[tryKey1]);

                                            // ------------
                                            // key1.second = key2.second /\ containPairBoolMap[<eqc(key1.first), eqc(key2.first)>]
                                            // ==>  (containPairBoolMap[key2] --> containPairBoolMap[key1])
                                            // ------------
                                            expr_ref implR(rewrite_implication(contain_pair_bool_map[key2], contain_pair_bool_map[key1]), m);
                                            assert_implication(mk_and(litems3), implR);
                                        }
                                    }

                                    {
                                        expr_ref_vector litems4(m);
                                        if (n1 != n2) {
                                            litems4.push_back(ctx.mk_eq_atom(n1, n2));
                                        }
                                        expr * eqStrVar1 = *eqItorStr1;
                                        if (eqStrVar1 != str1) {
                                            litems4.push_back(ctx.mk_eq_atom(str1, eqStrVar1));
                                        }
                                        expr *eqStrVar2 = *eqItorStr2;
                                        if (eqStrVar2 != str2) {
                                            litems4.push_back(ctx.mk_eq_atom(str2, eqStrVar2));
                                        }
                                        std::pair<expr*, expr*> tryKey2 = std::make_pair(eqStrVar2, eqStrVar1);

                                        if (contain_pair_bool_map.contains(tryKey2)) {
                                            TRACE("t_str_detail", tout << "(Contains " << mk_pp(eqStrVar2, m) << " " << mk_pp(eqStrVar1, m) << ")" << std::endl;);
                                            litems4.push_back(contain_pair_bool_map[tryKey2]);
                                            // ------------
                                            // key1.first = key2.first /\ containPairBoolMap[<eqc(key2.second), eqc(key1.second)>]
                                            // ==>  (containPairBoolMap[key1] --> containPairBoolMap[key2])
                                            // ------------
                                            expr_ref implR(rewrite_implication(contain_pair_bool_map[key1], contain_pair_bool_map[key2]), m);
                                            assert_implication(mk_and(litems4), implR);
                                        }
                                    }
                                }
                            }
                        }
                    }

                }
            }

            if (n1 == n2) {
                break;
            }
        }
    } // (in_contain_idx_map(n1) && in_contain_idx_map(n2))
}

void theory_str::check_contain_in_new_eq(expr * n1, expr * n2) {
    if (contains_map.empty()) {
        return;
    }

    context & ctx = get_context();
    ast_manager & m = get_manager();
    TRACE("t_str_detail", tout << "consistency check for contains wrt. " << mk_pp(n1, m) << " and " << mk_pp(n2, m) << std::endl;);

    expr_ref_vector willEqClass(m);
    expr * constStrAst_1 = collect_eq_nodes(n1, willEqClass);
    expr * constStrAst_2 = collect_eq_nodes(n2, willEqClass);
    expr * constStrAst = (constStrAst_1 != NULL) ? constStrAst_1 : constStrAst_2;

    TRACE("t_str_detail", tout << "eqc of n1 is {";
            for (expr_ref_vector::iterator it = willEqClass.begin(); it != willEqClass.end(); ++it) {
                expr * el = *it;
                tout << " " << mk_pp(el, m);
            }
            tout << std::endl;
            if (constStrAst == NULL) {
                tout << "constStrAst = NULL" << std::endl;
            } else {
                tout << "constStrAst = " << mk_pp(constStrAst, m) << std::endl;
            }
            );

    // step 1: we may have constant values for Contains checks now
    if (constStrAst != NULL) {
        expr_ref_vector::iterator itAst = willEqClass.begin();
        for (; itAst != willEqClass.end(); itAst++) {
            if (*itAst == constStrAst) {
                continue;
            }
            check_contain_by_eqc_val(*itAst, constStrAst);
        }
    } else {
        // no concrete value to be put in eqc, solely based on context
        // Check here is used to detected the facts as follows:
        //   * known: contains(Z, Y) /\ Z = "abcdefg" /\ Y = M
        //   * new fact: M = concat(..., "jio", ...)
        // Note that in this branch, either M or concat(..., "jio", ...) has a constant value
        // So, only need to check
        //   * "EQC(M) U EQC(concat(..., "jio", ...))" as substr and
        //   * If strAst registered has an eqc constant in the context
        // -------------------------------------------------------------
        expr_ref_vector::iterator itAst = willEqClass.begin();
        for (; itAst != willEqClass.end(); ++itAst) {
            check_contain_by_substr(*itAst, willEqClass);
        }
    }

    // ------------------------------------------
    // step 2: check for b1 = contains(x, m), b2 = contains(y, n)
    //         (1) x = y /\ m = n  ==>  b1 = b2
    //         (2) x = y /\ Contains(const(m), const(n))  ==>  (b1 -> b2)
    //         (3) x = y /\ Contains(const(n), const(m))  ==>  (b2 -> b1)
    //         (4) x = y /\ containPairBoolMap[<eqc(m), eqc(n)>]  ==>  (b1 -> b2)
    //         (5) x = y /\ containPairBoolMap[<eqc(n), eqc(m)>]  ==>  (b2 -> b1)
    //         (6) Contains(const(x), const(y)) /\ m = n  ==>  (b2 -> b1)
    //         (7) Contains(const(y), const(x)) /\ m = n  ==>  (b1 -> b2)
    //         (8) containPairBoolMap[<eqc(x), eqc(y)>] /\ m = n  ==>  (b2 -> b1)
    //         (9) containPairBoolMap[<eqc(y), eqc(x)>] /\ m = n  ==>  (b1 -> b2)
    // ------------------------------------------

    expr_ref_vector::iterator varItor1 = willEqClass.begin();
    for (; varItor1 != willEqClass.end(); ++varItor1) {
        expr * varAst1 = *varItor1;
        expr_ref_vector::iterator varItor2 = varItor1;
        for (; varItor2 != willEqClass.end(); ++varItor2) {
            expr * varAst2 = *varItor2;
            check_contain_by_eq_nodes(varAst1, varAst2);
        }
    }
}

expr * theory_str::dealias_node(expr * node, std::map<expr*, expr*> & varAliasMap, std::map<expr*, expr*> & concatAliasMap) {
    if (variable_set.find(node) != variable_set.end()) {
        return get_alias_index_ast(varAliasMap, node);
    } else if (is_concat(to_app(node))) {
        return get_alias_index_ast(concatAliasMap, node);
    }
    return node;
}

void theory_str::get_grounded_concats(expr* node, std::map<expr*, expr*> & varAliasMap,
        std::map<expr*, expr*> & concatAliasMap, std::map<expr*, expr*> & varConstMap,
        std::map<expr*, expr*> & concatConstMap, std::map<expr*, std::map<expr*, int> > & varEqConcatMap,
        std::map<expr*, std::map<std::vector<expr*>, std::set<expr*> > > & groundedMap) {
    if (is_Unroll(to_app(node))) {
        return;
    }
    // **************************************************
    // first deAlias the node if it is a var or concat
    // **************************************************
    node = dealias_node(node, varAliasMap, concatAliasMap);

    if (groundedMap.find(node) != groundedMap.end()) {
        return;
    }

    // haven't computed grounded concats for "node" (de-aliased)
    // ---------------------------------------------------------

    context & ctx = get_context();
    ast_manager & m = get_manager();

    // const strings: node is de-aliased
    if (m_strutil.is_string(node)) {
        std::vector<expr*> concatNodes;
        concatNodes.push_back(node);
        groundedMap[node][concatNodes].clear();   // no condition
    }
    // Concat functions
    else if (is_concat(to_app(node))) {
        // if "node" equals to a constant string, thenjust push the constant into the concat vector
        // Again "node" has been de-aliased at the very beginning
        if (concatConstMap.find(node) != concatConstMap.end()) {
            std::vector<expr*> concatNodes;
            concatNodes.push_back(concatConstMap[node]);
            groundedMap[node][concatNodes].clear();
            groundedMap[node][concatNodes].insert(ctx.mk_eq_atom(node, concatConstMap[node]));
        }
        // node doesn't have eq constant value. Process its children.
        else {
            // merge arg0 and arg1
            expr * arg0 = to_app(node)->get_arg(0);
            expr * arg1 = to_app(node)->get_arg(1);
            expr * arg0DeAlias = dealias_node(arg0, varAliasMap, concatAliasMap);
            expr * arg1DeAlias = dealias_node(arg1, varAliasMap, concatAliasMap);
            get_grounded_concats(arg0DeAlias, varAliasMap, concatAliasMap, varConstMap, concatConstMap, varEqConcatMap, groundedMap);
            get_grounded_concats(arg1DeAlias, varAliasMap, concatAliasMap, varConstMap, concatConstMap, varEqConcatMap, groundedMap);

            std::map<std::vector<expr*>, std::set<expr*> >::iterator arg0_grdItor = groundedMap[arg0DeAlias].begin();
            std::map<std::vector<expr*>, std::set<expr*> >::iterator arg1_grdItor;
            for (; arg0_grdItor != groundedMap[arg0DeAlias].end(); arg0_grdItor++) {
                arg1_grdItor = groundedMap[arg1DeAlias].begin();
                for (; arg1_grdItor != groundedMap[arg1DeAlias].end(); arg1_grdItor++) {
                    std::vector<expr*> ndVec;
                    ndVec.insert(ndVec.end(), arg0_grdItor->first.begin(), arg0_grdItor->first.end());
                    int arg0VecSize = arg0_grdItor->first.size();
                    int arg1VecSize = arg1_grdItor->first.size();
                    if (arg0VecSize > 0 && arg1VecSize > 0 && m_strutil.is_string(arg0_grdItor->first[arg0VecSize - 1]) && m_strutil.is_string(arg1_grdItor->first[0])) {
                        ndVec.pop_back();
                        ndVec.push_back(mk_concat(arg0_grdItor->first[arg0VecSize - 1], arg1_grdItor->first[0]));
                        for (int i = 1; i < arg1VecSize; i++) {
                            ndVec.push_back(arg1_grdItor->first[i]);
                        }
                    } else {
                        ndVec.insert(ndVec.end(), arg1_grdItor->first.begin(), arg1_grdItor->first.end());
                    }
                    // only insert if we don't know "node = concat(ndVec)" since one set of condition leads to this is enough
                    if (groundedMap[node].find(ndVec) == groundedMap[node].end()) {
                        groundedMap[node][ndVec];
                        if (arg0 != arg0DeAlias) {
                            groundedMap[node][ndVec].insert(ctx.mk_eq_atom(arg0, arg0DeAlias));
                        }
                        groundedMap[node][ndVec].insert(arg0_grdItor->second.begin(), arg0_grdItor->second.end());

                        if (arg1 != arg1DeAlias) {
                            groundedMap[node][ndVec].insert(ctx.mk_eq_atom(arg1, arg1DeAlias));
                        }
                        groundedMap[node][ndVec].insert(arg1_grdItor->second.begin(), arg1_grdItor->second.end());
                    }
                }
            }
        }
    }
    // string variables
    else if (variable_set.find(node) != variable_set.end()) {
        // deAliasedVar = Constant
        if (varConstMap.find(node) != varConstMap.end()) {
            std::vector<expr*> concatNodes;
            concatNodes.push_back(varConstMap[node]);
            groundedMap[node][concatNodes].clear();
            groundedMap[node][concatNodes].insert(ctx.mk_eq_atom(node, varConstMap[node]));
        }
        // deAliasedVar = someConcat
        else if (varEqConcatMap.find(node) != varEqConcatMap.end()) {
            expr * eqConcat = varEqConcatMap[node].begin()->first;
            expr * deAliasedEqConcat = dealias_node(eqConcat, varAliasMap, concatAliasMap);
            get_grounded_concats(deAliasedEqConcat, varAliasMap, concatAliasMap, varConstMap, concatConstMap, varEqConcatMap, groundedMap);

            std::map<std::vector<expr*>, std::set<expr*> >::iterator grdItor = groundedMap[deAliasedEqConcat].begin();
            for (; grdItor != groundedMap[deAliasedEqConcat].end(); grdItor++) {
                std::vector<expr*> ndVec;
                ndVec.insert(ndVec.end(), grdItor->first.begin(), grdItor->first.end());
                // only insert if we don't know "node = concat(ndVec)" since one set of condition leads to this is enough
                if (groundedMap[node].find(ndVec) == groundedMap[node].end()) {
                    // condition: node = deAliasedEqConcat
                    groundedMap[node][ndVec].insert(ctx.mk_eq_atom(node, deAliasedEqConcat));
                    // appending conditions for "deAliasedEqConcat = CONCAT(ndVec)"
                    groundedMap[node][ndVec].insert(grdItor->second.begin(), grdItor->second.end());
                }
            }
        }
        // node (has been de-aliased) != constant && node (has been de-aliased) != any concat
        // just push in the deAliasedVar
        else {
            std::vector<expr*> concatNodes;
            concatNodes.push_back(node);
            groundedMap[node][concatNodes]; // TODO ???
        }
    }
}

void theory_str::print_grounded_concat(expr * node, std::map<expr*, std::map<std::vector<expr*>, std::set<expr*> > > & groundedMap) {
    ast_manager & m = get_manager();
    TRACE("t_str_detail", tout << mk_pp(node, m) << std::endl;);
    if (groundedMap.find(node) != groundedMap.end()) {
        std::map<std::vector<expr*>, std::set<expr*> >::iterator itor = groundedMap[node].begin();
        for (; itor != groundedMap[node].end(); ++itor) {
            TRACE("t_str_detail",
                tout << "\t[grounded] ";
                std::vector<expr*>::const_iterator vIt = itor->first.begin();
                for (; vIt != itor->first.end(); ++vIt) {
                    tout << mk_pp(*vIt, m) << ", ";
                }
                tout << std::endl;
                tout << "\t[condition] ";
                std::set<expr*>::iterator sIt = itor->second.begin();
                for (; sIt != itor->second.end(); sIt++) {
                    tout << mk_pp(*sIt, m) << ", ";
                }
                tout << std::endl;
            );
        }
    } else {
        TRACE("t_str_detail", tout << "not found" << std::endl;);
    }
}

bool theory_str::is_partial_in_grounded_concat(const std::vector<expr*> & strVec, const std::vector<expr*> & subStrVec) {
    int strCnt = strVec.size();
    int subStrCnt = subStrVec.size();

    if (strCnt == 0 || subStrCnt == 0) {
        return false;
    }

    // The assumption is that all consecutive constant strings are merged into one node
    if (strCnt < subStrCnt) {
        return false;
    }

    if (subStrCnt == 1) {
        if (m_strutil.is_string(subStrVec[0])) {
            std::string subStrVal = m_strutil.get_string_constant_value(subStrVec[0]);
            for (int i = 0; i < strCnt; i++) {
                if (m_strutil.is_string(strVec[i])) {
                    std::string strVal = m_strutil.get_string_constant_value(strVec[i]);
                    if (strVal.find(subStrVal) != std::string::npos) {
                        return true;
                    }
                }
            }
        } else {
            for (int i = 0; i < strCnt; i++) {
                if (strVec[i] == subStrVec[0]) {
                    return true;
                }
            }
        }
        return false;
    } else {
        for (int i = 0; i <= (strCnt - subStrCnt); i++) {
            // The first node in subStrVect should be
            //   * constant: a suffix of a note in strVec[i]
            //   * variable:
            bool firstNodesOK = true;
            if (m_strutil.is_string(subStrVec[0])) {
                std::string subStrHeadVal = m_strutil.get_string_constant_value(subStrVec[0]);
                if (m_strutil.is_string(strVec[i])) {
                    std::string strHeadVal = m_strutil.get_string_constant_value(strVec[i]);
                    if (strHeadVal.size() >= subStrHeadVal.size()) {
                        std::string suffix = strHeadVal.substr(strHeadVal.size() - subStrHeadVal.size(), subStrHeadVal.size());
                        if (suffix != subStrHeadVal) {
                            firstNodesOK = false;
                        }
                    } else {
                        firstNodesOK = false;
                    }
                } else {
                    if (subStrVec[0] != strVec[i]) {
                        firstNodesOK = false;
                    }
                }
            }
            if (!firstNodesOK) {
                continue;
            }

            // middle nodes
            bool midNodesOK = true;
            for (int j = 1; j < subStrCnt - 1; j++) {
                if (subStrVec[j] != strVec[i + j]) {
                    midNodesOK = false;
                    break;
                }
            }
            if (!midNodesOK) {
                continue;
            }

            // tail nodes
            int tailIdx = i + subStrCnt - 1;
            if (m_strutil.is_string(subStrVec[subStrCnt - 1])) {
                std::string subStrTailVal = m_strutil.get_string_constant_value(subStrVec[subStrCnt - 1]);
                if (m_strutil.is_string(strVec[tailIdx])) {
                    std::string strTailVal = m_strutil.get_string_constant_value(strVec[tailIdx]);
                    if (strTailVal.size() >= subStrTailVal.size()) {
                        std::string prefix = strTailVal.substr(0, subStrTailVal.size());
                        if (prefix == subStrTailVal) {
                            return true;
                        } else {
                            continue;
                        }
                    } else {
                        continue;
                    }
                }
            } else {
                if (subStrVec[subStrCnt - 1] == strVec[tailIdx]) {
                    return true;
                } else {
                    continue;
                }
            }
        }
        return false;
    }
}

void theory_str::check_subsequence(expr* str, expr* strDeAlias, expr* subStr, expr* subStrDeAlias, expr* boolVar,
    std::map<expr*, std::map<std::vector<expr*>, std::set<expr*> > > & groundedMap) {

    context & ctx = get_context();
    ast_manager & m = get_manager();
    std::map<std::vector<expr*>, std::set<expr*> >::iterator itorStr = groundedMap[strDeAlias].begin();
    std::map<std::vector<expr*>, std::set<expr*> >::iterator itorSubStr;
    for (; itorStr != groundedMap[strDeAlias].end(); itorStr++) {
        itorSubStr = groundedMap[subStrDeAlias].begin();
        for (; itorSubStr != groundedMap[subStrDeAlias].end(); itorSubStr++) {
            bool contain = is_partial_in_grounded_concat(itorStr->first, itorSubStr->first);
            if (contain) {
                expr_ref_vector litems(m);
                if (str != strDeAlias) {
                    litems.push_back(ctx.mk_eq_atom(str, strDeAlias));
                }
                if (subStr != subStrDeAlias) {
                    litems.push_back(ctx.mk_eq_atom(subStr, subStrDeAlias));
                }

                //litems.insert(itorStr->second.begin(), itorStr->second.end());
                //litems.insert(itorSubStr->second.begin(), itorSubStr->second.end());
                for (std::set<expr*>::const_iterator i1 = itorStr->second.begin();
                        i1 != itorStr->second.end(); ++i1) {
                    litems.push_back(*i1);
                }
                for (std::set<expr*>::const_iterator i1 = itorSubStr->second.begin();
                        i1 != itorSubStr->second.end(); ++i1) {
                    litems.push_back(*i1);
                }

                expr_ref implyR(boolVar, m);

                if (litems.empty()) {
                    assert_axiom(implyR);
                } else {
                    expr_ref implyL(mk_and(litems), m);
                    assert_implication(implyL, implyR);
                }

            }
        }
    }
}

void theory_str::compute_contains(std::map<expr*, expr*> & varAliasMap,
                std::map<expr*, expr*> & concatAliasMap, std::map<expr*, expr*> & varConstMap,
                std::map<expr*, expr*> & concatConstMap, std::map<expr*, std::map<expr*, int> > & varEqConcatMap) {
    std::map<expr*, std::map<std::vector<expr*>, std::set<expr*> > > groundedMap;
    theory_str_contain_pair_bool_map_t::iterator containItor = contain_pair_bool_map.begin();
    for (; containItor != contain_pair_bool_map.end(); containItor++) {
        expr* containBoolVar = containItor->get_value();
        expr* str = containItor->get_key1();
        expr* subStr = containItor->get_key2();

        expr* strDeAlias = dealias_node(str, varAliasMap, concatAliasMap);
        expr* subStrDeAlias = dealias_node(subStr, varAliasMap, concatAliasMap);

        get_grounded_concats(strDeAlias, varAliasMap, concatAliasMap, varConstMap, concatConstMap, varEqConcatMap, groundedMap);
        get_grounded_concats(subStrDeAlias, varAliasMap, concatAliasMap, varConstMap, concatConstMap, varEqConcatMap, groundedMap);

        // debugging
        print_grounded_concat(strDeAlias, groundedMap);
        print_grounded_concat(subStrDeAlias, groundedMap);

        check_subsequence(str, strDeAlias, subStr, subStrDeAlias, containBoolVar, groundedMap);
    }
}

bool theory_str::can_concat_eq_str(expr * concat, std::string str) {
	// TODO this method could use some traces and debugging info
	int strLen = str.length();
	if (is_concat(to_app(concat))) {
		ptr_vector<expr> args;
		get_nodes_in_concat(concat, args);
		expr * ml_node = args[0];
		expr * mr_node = args[args.size() - 1];

		if (m_strutil.is_string(ml_node)) {
			std::string ml_str = m_strutil.get_string_constant_value(ml_node);
			int ml_len = ml_str.length();
			if (ml_len > strLen) {
				return false;
			}
			int cLen = ml_len;
			if (ml_str != str.substr(0, cLen)) {
				return false;
			}
		}

		if (m_strutil.is_string(mr_node)) {
			std::string mr_str = m_strutil.get_string_constant_value(mr_node);
			int mr_len = mr_str.length();
			if (mr_len > strLen) {
				return false;
			}
			int cLen = mr_len;
			if (mr_str != str.substr(strLen - cLen, cLen)) {
				return false;
			}
		}

		int sumLen = 0;
		for (unsigned int i = 0 ; i < args.size() ; i++) {
			expr * oneArg = args[i];
			if (m_strutil.is_string(oneArg)) {
				std::string arg_str = m_strutil.get_string_constant_value(oneArg);
				if (str.find(arg_str) == std::string::npos) {
					return false;
				}
				sumLen += arg_str.length();
			}
		}

		if (sumLen > strLen) {
			return false;
		}
	}
	return true;
}

bool theory_str::can_concat_eq_concat(expr * concat1, expr * concat2) {
	// TODO this method could use some traces and debugging info
	if (is_concat(to_app(concat1)) && is_concat(to_app(concat2))) {
		{
			// Suppose concat1 = (Concat X Y) and concat2 = (Concat M N).
			expr * concat1_mostL = getMostLeftNodeInConcat(concat1);
			expr * concat2_mostL = getMostLeftNodeInConcat(concat2);
			// if both X and M are constant strings, check whether they have the same prefix
			if (m_strutil.is_string(concat1_mostL) && m_strutil.is_string(concat2_mostL)) {
				std::string concat1_mostL_str = m_strutil.get_string_constant_value(concat1_mostL);
				std::string concat2_mostL_str = m_strutil.get_string_constant_value(concat2_mostL);
				int cLen = std::min(concat1_mostL_str.length(), concat2_mostL_str.length());
				if (concat1_mostL_str.substr(0, cLen) != concat2_mostL_str.substr(0, cLen)) {
					return false;
				}
			}
		}

		{
			// Similarly, if both Y and N are constant strings, check whether they have the same suffix
			expr * concat1_mostR = getMostRightNodeInConcat(concat1);
			expr * concat2_mostR = getMostRightNodeInConcat(concat2);
			if (m_strutil.is_string(concat1_mostR) && m_strutil.is_string(concat2_mostR)) {
				std::string concat1_mostR_str = m_strutil.get_string_constant_value(concat1_mostR);
				std::string concat2_mostR_str = m_strutil.get_string_constant_value(concat2_mostR);
				int cLen = std::min(concat1_mostR_str.length(), concat2_mostR_str.length());
				if (concat1_mostR_str.substr(concat1_mostR_str.length() - cLen, cLen) !=
						concat2_mostR_str.substr(concat2_mostR_str.length() - cLen, cLen)) {
					return false;
				}
			}
		}
	}
	return true;
}

/*
 * Check whether n1 and n2 could be equal.
 * Returns true if n1 could equal n2 (maybe),
 * and false if n1 is definitely not equal to n2 (no).
 */
bool theory_str::can_two_nodes_eq(expr * n1, expr * n2) {
    app * n1_curr = to_app(n1);
    app * n2_curr = to_app(n2);

    // case 0: n1_curr is const string, n2_curr is const string
    if (is_string(n1_curr) && is_string(n2_curr)) {
      if (n1_curr != n2_curr) {
        return false;
      }
    }
    // case 1: n1_curr is concat, n2_curr is const string
    else if (is_concat(n1_curr) && is_string(n2_curr)) {
        const char * tmp = 0;
        m_strutil.is_string(n2_curr, & tmp);
        std::string n2_curr_str(tmp);
        if (!can_concat_eq_str(n1_curr, n2_curr_str)) {
            return false;
        }
    }
    // case 2: n2_curr is concat, n1_curr is const string
    else if (is_concat(n2_curr) && is_string(n1_curr)) {
        const char * tmp = 0;
        m_strutil.is_string(n1_curr, & tmp);
        std::string n1_curr_str(tmp);
        if (!can_concat_eq_str(n2_curr, n1_curr_str)) {
            return false;
        }
    }
    // case 3: both are concats
    else if (is_concat(n1_curr) && is_concat(n2_curr)) {
      if (!can_concat_eq_concat(n1_curr, n2_curr)) {
        return false;
      }
    }

    return true;
}

// was checkLength2ConstStr() in Z3str2
// returns true if everything is OK, or false if inconsistency detected
// - note that these are different from the semantics in Z3str2
bool theory_str::check_length_const_string(expr * n1, expr * constStr) {
    ast_manager & mgr = get_manager();
    context & ctx = get_context();

    rational strLen((unsigned) (m_strutil.get_string_constant_value(constStr).length()));

    if (is_concat(to_app(n1))) {
        ptr_vector<expr> args;
        expr_ref_vector items(mgr);

        get_nodes_in_concat(n1, args);

        rational sumLen(0);
        for (unsigned int i = 0; i < args.size(); ++i) {
            rational argLen;
            bool argLen_exists = get_len_value(args[i], argLen);
            if (argLen_exists) {
                if (!m_strutil.is_string(args[i])) {
                    items.push_back(ctx.mk_eq_atom(mk_strlen(args[i]), mk_int(argLen)));
                }
                TRACE("t_str_detail", tout << "concat arg: " << mk_pp(args[i], mgr) << " has len = " << argLen.to_string() << std::endl;);
                sumLen += argLen;
                if (sumLen > strLen) {
                    items.push_back(ctx.mk_eq_atom(n1, constStr));
                    expr_ref toAssert(mgr.mk_not(mk_and(items)), mgr);
                    TRACE("t_str_detail", tout << "inconsistent length: concat (len = " << sumLen << ") <==> string constant (len = " << strLen << ")" << std::endl;);
                    assert_axiom(toAssert);
                    return false;
                }
            }
        }
    } else { // !is_concat(n1)
        rational oLen;
        bool oLen_exists = get_len_value(n1, oLen);
        if (oLen_exists && oLen != strLen) {
            TRACE("t_str_detail", tout << "inconsistent length: var (len = " << oLen << ") <==> string constant (len = " << strLen << ")" << std::endl;);
            expr_ref l(ctx.mk_eq_atom(n1, constStr), mgr);
            expr_ref r(ctx.mk_eq_atom(mk_strlen(n1), mk_strlen(constStr)), mgr);
            assert_implication(l, r);
            return false;
        }
    }
    rational unused;
    if (get_len_value(n1, unused) == false) {
        expr_ref l(ctx.mk_eq_atom(n1, constStr), mgr);
        expr_ref r(ctx.mk_eq_atom(mk_strlen(n1), mk_strlen(constStr)), mgr);
        assert_implication(l, r);
    }
    return true;
}

bool theory_str::check_length_concat_concat(expr * n1, expr * n2) {
    context & ctx = get_context();
    ast_manager & mgr = get_manager();

    ptr_vector<expr> concat1Args;
    ptr_vector<expr> concat2Args;
    get_nodes_in_concat(n1, concat1Args);
    get_nodes_in_concat(n2, concat2Args);

    bool concat1LenFixed = true;
    bool concat2LenFixed = true;

    expr_ref_vector items(mgr);

    rational sum1(0), sum2(0);

    for (unsigned int i = 0; i < concat1Args.size(); ++i) {
        expr * oneArg = concat1Args[i];
        rational argLen;
        bool argLen_exists = get_len_value(oneArg, argLen);
        if (argLen_exists) {
            sum1 += argLen;
            if (!m_strutil.is_string(oneArg)) {
                items.push_back(ctx.mk_eq_atom(mk_strlen(oneArg), mk_int(argLen)));
            }
        } else {
            concat1LenFixed = false;
        }
    }

    for (unsigned int i = 0; i < concat2Args.size(); ++i) {
        expr * oneArg = concat2Args[i];
        rational argLen;
        bool argLen_exists = get_len_value(oneArg, argLen);
        if (argLen_exists) {
            sum2 += argLen;
            if (!m_strutil.is_string(oneArg)) {
                items.push_back(ctx.mk_eq_atom(mk_strlen(oneArg), mk_int(argLen)));
            }
        } else {
            concat2LenFixed = false;
        }
    }

    items.push_back(ctx.mk_eq_atom(n1, n2));

    bool conflict = false;

    if (concat1LenFixed && concat2LenFixed) {
        if (sum1 != sum2) {
            conflict = true;
        }
    } else if (!concat1LenFixed && concat2LenFixed) {
        if (sum1 > sum2) {
            conflict = true;
        }
    } else if (concat1LenFixed && !concat2LenFixed) {
        if (sum1 < sum2) {
            conflict = true;
        }
    }

    if (conflict) {
        TRACE("t_str_detail", tout << "inconsistent length detected in concat <==> concat" << std::endl;);
        expr_ref toAssert(mgr.mk_not(mk_and(items)), mgr);
        assert_axiom(toAssert);
        return false;
    }
    return true;
}

bool theory_str::check_length_concat_var(expr * concat, expr * var) {
    context & ctx = get_context();
    ast_manager & mgr = get_manager();

    rational varLen;
    bool varLen_exists = get_len_value(var, varLen);
    if (!varLen_exists) {
        return true;
    } else {
        rational sumLen(0);
        ptr_vector<expr> args;
        expr_ref_vector items(mgr);
        get_nodes_in_concat(concat, args);
        for (unsigned int i = 0; i < args.size(); ++i) {
            expr * oneArg = args[i];
            rational argLen;
            bool argLen_exists = get_len_value(oneArg, argLen);
            if (argLen_exists) {
                if (!m_strutil.is_string(oneArg) && !argLen.is_zero()) {
                    items.push_back(ctx.mk_eq_atom(mk_strlen(oneArg), mk_int(argLen)));
                }
                sumLen += argLen;
                if (sumLen > varLen) {
                    TRACE("t_str_detail", tout << "inconsistent length detected in concat <==> var" << std::endl;);
                    items.push_back(ctx.mk_eq_atom(mk_strlen(var), mk_int(varLen)));
                    items.push_back(ctx.mk_eq_atom(concat, var));
                    expr_ref toAssert(mgr.mk_not(mk_and(items)), mgr);
                    assert_axiom(toAssert);
                    return false;
                }
            }
        }
        return true;
    }
}

bool theory_str::check_length_var_var(expr * var1, expr * var2) {
    context & ctx = get_context();
    ast_manager & mgr = get_manager();

    rational var1Len, var2Len;
    bool var1Len_exists = get_len_value(var1, var1Len);
    bool var2Len_exists = get_len_value(var2, var2Len);

    if (var1Len_exists && var2Len_exists && var1Len != var2Len) {
        TRACE("t_str_detail", tout << "inconsistent length detected in var <==> var" << std::endl;);
        expr_ref_vector items(mgr);
        items.push_back(ctx.mk_eq_atom(mk_strlen(var1), mk_int(var1Len)));
        items.push_back(ctx.mk_eq_atom(mk_strlen(var2), mk_int(var2Len)));
        items.push_back(ctx.mk_eq_atom(var1, var2));
        expr_ref toAssert(mgr.mk_not(mk_and(items)), mgr);
        assert_axiom(toAssert);
        return false;
    }
    return true;
}

// returns true if everything is OK, or false if inconsistency detected
// - note that these are different from the semantics in Z3str2
bool theory_str::check_length_eq_var_concat(expr * n1, expr * n2) {
    // n1 and n2 are not const string: either variable or concat
    bool n1Concat = is_concat(to_app(n1));
    bool n2Concat = is_concat(to_app(n2));
    if (n1Concat && n2Concat) {
        return check_length_concat_concat(n1, n2);
    }
    // n1 is concat, n2 is variable
    else if (n1Concat && (!n2Concat)) {
        return check_length_concat_var(n1, n2);
    }
    // n1 is variable, n2 is concat
    else if ((!n1Concat) && n2Concat) {
        return check_length_concat_var(n2, n1);
    }
    // n1 and n2 are both variables
    else {
        return check_length_var_var(n1, n2);
    }
    return true;
}

// returns false if an inconsistency is detected, or true if no inconsistencies were found
// - note that these are different from the semantics of checkLengConsistency() in Z3str2
bool theory_str::check_length_consistency(expr * n1, expr * n2) {
	if (m_strutil.is_string(n1) && m_strutil.is_string(n2)) {
		// consistency has already been checked in can_two_nodes_eq().
		return true;
	} else if (m_strutil.is_string(n1) && (!m_strutil.is_string(n2))) {
		return check_length_const_string(n2, n1);
	} else if (m_strutil.is_string(n2) && (!m_strutil.is_string(n1))) {
		return check_length_const_string(n1, n2);
	} else {
		// n1 and n2 are vars or concats
		return check_length_eq_var_concat(n1, n2);
	}
	return true;
}

// Modified signature: returns true if nothing was learned, or false if at least one axiom was asserted.
// (This is used for deferred consistency checking)
bool theory_str::check_concat_len_in_eqc(expr * concat) {
    context & ctx = get_context();

    bool no_assertions = true;

    expr * eqc_n = concat;
    do {
        if (is_concat(to_app(eqc_n))) {
            rational unused;
            bool status = infer_len_concat(eqc_n, unused);
            if (status) {
                no_assertions = false;
            }
        }
        eqc_n = get_eqc_next(eqc_n);
    } while (eqc_n != concat);

    return no_assertions;
}

void theory_str::check_regex_in(expr * nn1, expr * nn2) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    expr_ref_vector eqNodeSet(m);

    expr * constStr_1 = collect_eq_nodes(nn1, eqNodeSet);
    expr * constStr_2 = collect_eq_nodes(nn2, eqNodeSet);
    expr * constStr = (constStr_1 != NULL) ? constStr_1 : constStr_2;

    if (constStr == NULL) {
        return;
    } else {
        expr_ref_vector::iterator itor = eqNodeSet.begin();
        for (; itor != eqNodeSet.end(); itor++) {
            if (regex_in_var_reg_str_map.find(*itor) != regex_in_var_reg_str_map.end()) {
                std::set<std::string>::iterator strItor = regex_in_var_reg_str_map[*itor].begin();
                for (; strItor != regex_in_var_reg_str_map[*itor].end(); strItor++) {
                    std::string regStr = *strItor;
                    std::string constStrValue = m_strutil.get_string_constant_value(constStr);
                    std::pair<expr*, std::string> key1 = std::make_pair(*itor, regStr);
                    if (regex_in_bool_map.find(key1) != regex_in_bool_map.end()) {
                        expr * boolVar = regex_in_bool_map[key1]; // actually the RegexIn term
                        app * a_regexIn = to_app(boolVar);
                        expr * regexTerm = a_regexIn->get_arg(1);

                        if (regex_nfa_cache.find(regexTerm) == regex_nfa_cache.end()) {
                            TRACE("t_str_detail", tout << "regex_nfa_cache: cache miss" << std::endl;);
                            regex_nfa_cache[regexTerm] = nfa(m_strutil, regexTerm);
                        } else {
                            TRACE("t_str_detail", tout << "regex_nfa_cache: cache hit" << std::endl;);
                        }

                        nfa regexNFA = regex_nfa_cache[regexTerm];
                        ENSURE(regexNFA.is_valid());
                        bool matchRes = regexNFA.matches(constStrValue);

                        TRACE("t_str_detail", tout << mk_pp(*itor, m) << " in " << regStr << " : " << (matchRes ? "yes" : "no") << std::endl;);

                        expr_ref implyL(ctx.mk_eq_atom(*itor, constStr), m);
                        if (matchRes) {
                            assert_implication(implyL, boolVar);
                        } else {
                            assert_implication(implyL, m.mk_not(boolVar));
                        }
                    }
                }
            }
        }
    }
}

/*
 * strArgmt::solve_concat_eq_str()
 * Solve concatenations of the form:
 *   const == Concat(const, X)
 *   const == Concat(X, const)
 */
void theory_str::solve_concat_eq_str(expr * concat, expr * str) {
    ast_manager & m = get_manager();
    context & ctx = get_context();

    TRACE("t_str_detail", tout << mk_ismt2_pp(concat, m) << " == " << mk_ismt2_pp(str, m) << std::endl;);

    if (is_concat(to_app(concat)) && is_string(to_app(str))) {
        const char * tmp = 0;
        m_strutil.is_string(str, & tmp);
        std::string const_str(tmp);
        app * a_concat = to_app(concat);
        SASSERT(a_concat->get_num_args() == 2);
        expr * a1 = a_concat->get_arg(0);
        expr * a2 = a_concat->get_arg(1);

        if (const_str == "") {
            TRACE("t_str", tout << "quick path: concat == \"\"" << std::endl;);
            // assert the following axiom:
            // ( (Concat a1 a2) == "" ) -> ( (a1 == "") AND (a2 == "") )


            expr_ref premise(ctx.mk_eq_atom(concat, str), m);
            expr_ref c1(ctx.mk_eq_atom(a1, str), m);
            expr_ref c2(ctx.mk_eq_atom(a2, str), m);
            expr_ref conclusion(m.mk_and(c1, c2), m);
            assert_implication(premise, conclusion);

            return;
        }
        bool arg1_has_eqc_value = false;
        bool arg2_has_eqc_value = false;
        expr * arg1 = get_eqc_value(a1, arg1_has_eqc_value);
        expr * arg2 = get_eqc_value(a2, arg2_has_eqc_value);
        expr_ref newConcat(m);
        if (arg1 != a1 || arg2 != a2) {
        	TRACE("t_str", tout << "resolved concat argument(s) to eqc string constants" << std::endl;);
        	int iPos = 0;
        	app * item1[2];
        	if (a1 != arg1) {
        		item1[iPos++] = ctx.mk_eq_atom(a1, arg1);
        	}
        	if (a2 != arg2) {
        		item1[iPos++] = ctx.mk_eq_atom(a2, arg2);
        	}
        	expr_ref implyL1(m);
        	if (iPos == 1) {
        		implyL1 = item1[0];
        	} else {
        		implyL1 = m.mk_and(item1[0], item1[1]);
        	}
        	newConcat = mk_concat(arg1, arg2);
        	if (newConcat != str) {
        		expr_ref implyR1(ctx.mk_eq_atom(concat, newConcat), m);
        		assert_implication(implyL1, implyR1);
        	}
        } else {
        	newConcat = concat;
        }
        if (newConcat == str) {
        	return;
        }
        if (!is_concat(to_app(newConcat))) {
        	return;
        }
        if (arg1_has_eqc_value && arg2_has_eqc_value) {
        	// Case 1: Concat(const, const) == const
        	TRACE("t_str", tout << "Case 1: Concat(const, const) == const" << std::endl;);
        	const char * str1;
        	m_strutil.is_string(arg1, & str1);
        	std::string arg1_str(str1);

        	const char * str2;
        	m_strutil.is_string(arg2, & str2);
        	std::string arg2_str(str2);

        	std::string result_str = arg1_str + arg2_str;
        	if (result_str != const_str) {
        		// Inconsistency
        		TRACE("t_str", tout << "inconsistency detected: \""
        				<< arg1_str << "\" + \"" << arg2_str <<
						"\" != \"" << const_str << "\"" << std::endl;);
        		expr_ref equality(ctx.mk_eq_atom(concat, str), m);
        		expr_ref diseq(m.mk_not(equality), m);
        		assert_axiom(diseq);
        		return;
        	}
        } else if (!arg1_has_eqc_value && arg2_has_eqc_value) {
        	// Case 2: Concat(var, const) == const
        	TRACE("t_str", tout << "Case 2: Concat(var, const) == const" << std::endl;);
        	const char * str2;
			m_strutil.is_string(arg2, & str2);
			std::string arg2_str(str2);
			int resultStrLen = const_str.length();
			int arg2StrLen = arg2_str.length();
			if (resultStrLen < arg2StrLen) {
				// Inconsistency
				TRACE("t_str", tout << "inconsistency detected: \""
						 << arg2_str <<
						"\" is longer than \"" << const_str << "\","
						<< " so cannot be concatenated with anything to form it" << std::endl;);
				expr_ref equality(ctx.mk_eq_atom(newConcat, str), m);
				expr_ref diseq(m.mk_not(equality), m);
				assert_axiom(diseq);
				return;
			} else {
				int varStrLen = resultStrLen - arg2StrLen;
				std::string firstPart = const_str.substr(0, varStrLen);
				std::string secondPart = const_str.substr(varStrLen, arg2StrLen);
				if (arg2_str != secondPart) {
					// Inconsistency
					TRACE("t_str", tout << "inconsistency detected: "
							<< "suffix of concatenation result expected \"" << secondPart << "\", "
							<< "actually \"" << arg2_str << "\""
							<< std::endl;);
					expr_ref equality(ctx.mk_eq_atom(newConcat, str), m);
					expr_ref diseq(m.mk_not(equality), m);
					assert_axiom(diseq);
					return;
				} else {
					expr_ref tmpStrConst(m_strutil.mk_string(firstPart), m);
					expr_ref premise(ctx.mk_eq_atom(newConcat, str), m);
					expr_ref conclusion(ctx.mk_eq_atom(arg1, tmpStrConst), m);
					assert_implication(premise, conclusion);
					return;
				}
			}
        } else if (arg1_has_eqc_value && !arg2_has_eqc_value) {
        	// Case 3: Concat(const, var) == const
        	TRACE("t_str", tout << "Case 3: Concat(const, var) == const" << std::endl;);
        	const char * str1;
			m_strutil.is_string(arg1, & str1);
			std::string arg1_str(str1);
			int resultStrLen = const_str.length();
			int arg1StrLen = arg1_str.length();
			if (resultStrLen < arg1StrLen) {
				// Inconsistency
				TRACE("t_str", tout << "inconsistency detected: \""
						 << arg1_str <<
						"\" is longer than \"" << const_str << "\","
						<< " so cannot be concatenated with anything to form it" << std::endl;);
				expr_ref equality(ctx.mk_eq_atom(newConcat, str), m);
				expr_ref diseq(m.mk_not(equality), m);
				assert_axiom(diseq);
				return;
			} else {
				int varStrLen = resultStrLen - arg1StrLen;
				std::string firstPart = const_str.substr(0, arg1StrLen);
				std::string secondPart = const_str.substr(arg1StrLen, varStrLen);
				if (arg1_str != firstPart) {
					// Inconsistency
					TRACE("t_str", tout << "inconsistency detected: "
							<< "prefix of concatenation result expected \"" << secondPart << "\", "
							<< "actually \"" << arg1_str << "\""
							<< std::endl;);
					expr_ref equality(ctx.mk_eq_atom(newConcat, str), m);
					expr_ref diseq(m.mk_not(equality), m);
					assert_axiom(diseq);
					return;
				} else {
					expr_ref tmpStrConst(m_strutil.mk_string(secondPart), m);
					expr_ref premise(ctx.mk_eq_atom(newConcat, str), m);
					expr_ref conclusion(ctx.mk_eq_atom(arg2, tmpStrConst), m);
					assert_implication(premise, conclusion);
					return;
				}
			}
        } else {
        	// Case 4: Concat(var, var) == const
        	TRACE("t_str", tout << "Case 4: Concat(var, var) == const" << std::endl;);
        	if (eval_concat(arg1, arg2) == NULL) {
        	    rational arg1Len, arg2Len;
        	    bool arg1Len_exists = get_len_value(arg1, arg1Len);
        	    bool arg2Len_exists = get_len_value(arg2, arg2Len);
        	    rational concatStrLen((unsigned)const_str.length());
        		if (arg1Len_exists || arg2Len_exists) {
        		    expr_ref ax_l1(ctx.mk_eq_atom(concat, str), m);
        		    expr_ref ax_l2(m);
        		    std::string prefixStr, suffixStr;
        		    if (arg1Len_exists) {
        		        if (arg1Len.is_neg()) {
        		            TRACE("t_str_detail", tout << "length conflict: arg1Len = " << arg1Len << ", concatStrLen = " << concatStrLen << std::endl;);
        		            expr_ref toAssert(m_autil.mk_ge(mk_strlen(arg1), mk_int(0)), m);
        		            assert_axiom(toAssert);
        		            return;
        		        } else if (arg1Len > concatStrLen) {
        		            TRACE("t_str_detail", tout << "length conflict: arg1Len = " << arg1Len << ", concatStrLen = " << concatStrLen << std::endl;);
        		            expr_ref ax_r1(m_autil.mk_le(mk_strlen(arg1), mk_int(concatStrLen)), m);
        		            assert_implication(ax_l1, ax_r1);
        		            return;
        		        }

        		        prefixStr = const_str.substr(0, arg1Len.get_unsigned());
        		        rational concat_minus_arg1 = concatStrLen - arg1Len;
        		        suffixStr = const_str.substr(arg1Len.get_unsigned(), concat_minus_arg1.get_unsigned());
        		        ax_l2 = ctx.mk_eq_atom(mk_strlen(arg1), mk_int(arg1Len));
        		    } else {
        		        // arg2's length is available
        		        if (arg2Len.is_neg()) {
        		            TRACE("t_str_detail", tout << "length conflict: arg2Len = " << arg2Len << ", concatStrLen = " << concatStrLen << std::endl;);
        		            expr_ref toAssert(m_autil.mk_ge(mk_strlen(arg2), mk_int(0)), m);
        		            assert_axiom(toAssert);
        		            return;
        		        } else if (arg2Len > concatStrLen) {
        		            TRACE("t_str_detail", tout << "length conflict: arg2Len = " << arg2Len << ", concatStrLen = " << concatStrLen << std::endl;);
        		            expr_ref ax_r1(m_autil.mk_le(mk_strlen(arg2), mk_int(concatStrLen)), m);
        		            assert_implication(ax_l1, ax_r1);
        		            return;
        		        }

        		        rational concat_minus_arg2 = concatStrLen - arg2Len;
        		        prefixStr = const_str.substr(0, concat_minus_arg2.get_unsigned());
        		        suffixStr = const_str.substr(concat_minus_arg2.get_unsigned(), arg2Len.get_unsigned());
        		        ax_l2 = ctx.mk_eq_atom(mk_strlen(arg2), mk_int(arg2Len));
        		    }
        		    // consistency check
        		    if (is_concat(to_app(arg1)) && !can_concat_eq_str(arg1, prefixStr)) {
        		        expr_ref ax_r(m.mk_not(ax_l2), m);
        		        assert_implication(ax_l1, ax_r);
        		        return;
        		    }
        		    if (is_concat(to_app(arg2)) && !can_concat_eq_str(arg2, suffixStr)) {
        		        expr_ref ax_r(m.mk_not(ax_l2), m);
        		        assert_implication(ax_l1, ax_r);
        		        return;
        		    }
        		    expr_ref_vector r_items(m);
        		    r_items.push_back(ctx.mk_eq_atom(arg1, m_strutil.mk_string(prefixStr)));
        		    r_items.push_back(ctx.mk_eq_atom(arg2, m_strutil.mk_string(suffixStr)));
        		    if (!arg1Len_exists) {
        		        r_items.push_back(ctx.mk_eq_atom(mk_strlen(arg1), mk_int(prefixStr.size())));
        		    }
        		    if (!arg2Len_exists) {
        		        r_items.push_back(ctx.mk_eq_atom(mk_strlen(arg2), mk_int(suffixStr.size())));
        		    }
        		    expr_ref lhs(m.mk_and(ax_l1, ax_l2), m);
        		    expr_ref rhs(mk_and(r_items), m);
        		    assert_implication(lhs, rhs);
        		} else { /* ! (arg1Len != 1 || arg2Len != 1) */
        			expr_ref xorFlag(m);
        			std::pair<expr*, expr*> key1(arg1, arg2);
        			std::pair<expr*, expr*> key2(arg2, arg1);

        			// check the entries in this map to make sure they're still in scope
        			// before we use them.

        			// TODO XOR variables will always show up as "not in scope" because of how we update internal_variable_set

        			std::map<std::pair<expr*,expr*>, std::map<int, expr*> >::iterator entry1 = varForBreakConcat.find(key1);
        			std::map<std::pair<expr*,expr*>, std::map<int, expr*> >::iterator entry2 = varForBreakConcat.find(key2);

        			bool entry1InScope;
        			if (entry1 == varForBreakConcat.end()) {
        			    TRACE("t_str_detail", tout << "key1 no entry" << std::endl;);
        			    entry1InScope = false;
        			} else {
        			    // OVERRIDE.
        			    entry1InScope = true;
        			    TRACE("t_str_detail", tout << "key1 entry" << std::endl;);
        			    /*
        			    if (internal_variable_set.find((entry1->second)[0]) == internal_variable_set.end()) {
        			        TRACE("t_str_detail", tout << "key1 entry not in scope" << std::endl;);
        			        entry1InScope = false;
        			    } else {
        			        TRACE("t_str_detail", tout << "key1 entry in scope" << std::endl;);
        			        entry1InScope = true;
        			    }
        			    */
        			}

        			bool entry2InScope;
        			if (entry2 == varForBreakConcat.end()) {
        			    TRACE("t_str_detail", tout << "key2 no entry" << std::endl;);
        			    entry2InScope = false;
        			} else {
        			    // OVERRIDE.
        			    entry2InScope = true;
        			    TRACE("t_str_detail", tout << "key2 entry" << std::endl;);
        			    /*
        			    if (internal_variable_set.find((entry2->second)[0]) == internal_variable_set.end()) {
        			        TRACE("t_str_detail", tout << "key2 entry not in scope" << std::endl;);
        			        entry2InScope = false;
        			    } else {
        			        TRACE("t_str_detail", tout << "key2 entry in scope" << std::endl;);
        			        entry2InScope = true;
        			    }
        			    */
        			}

        			TRACE("t_str_detail", tout << "entry 1 " << (entry1InScope ? "in scope" : "not in scope") << std::endl
        			        << "entry 2 " << (entry2InScope ? "in scope" : "not in scope") << std::endl;);

        			if (!entry1InScope && !entry2InScope) {
        				xorFlag = mk_internal_xor_var();
        				varForBreakConcat[key1][0] = xorFlag;
        			} else if (entry1InScope) {
        				xorFlag = varForBreakConcat[key1][0];
        			} else { // entry2InScope
        				xorFlag = varForBreakConcat[key2][0];
        			}

        			int concatStrLen = const_str.length();
        			int xor_pos = 0;
        			int and_count = 1;

        			expr ** xor_items = alloc_svect(expr*, (concatStrLen+1));
        			expr ** and_items = alloc_svect(expr*, (4 * (concatStrLen+1) + 1));

        			for (int i = 0; i < concatStrLen + 1; ++i) {
        				std::string prefixStr = const_str.substr(0, i);
        				std::string suffixStr = const_str.substr(i, concatStrLen - i);
        				// skip invalid options
        				if (is_concat(to_app(arg1)) && !can_concat_eq_str(arg1, prefixStr)) {
        				    continue;
        				}
        				if (is_concat(to_app(arg2)) && !can_concat_eq_str(arg2, suffixStr)) {
        				    continue;
        				}
        				expr_ref xorAst(ctx.mk_eq_atom(xorFlag, m_autil.mk_numeral(rational(xor_pos), true)), m);
        				xor_items[xor_pos++] = xorAst;

        				expr_ref prefixAst(m_strutil.mk_string(prefixStr), m);
        				expr_ref arg1_eq (ctx.mk_eq_atom(arg1, prefixAst), m);
        				and_items[and_count++] = ctx.mk_eq_atom(xorAst, arg1_eq);

        				expr_ref suffixAst(m_strutil.mk_string(suffixStr), m);
        				expr_ref arg2_eq (ctx.mk_eq_atom(arg2, suffixAst), m);
        				and_items[and_count++] = ctx.mk_eq_atom(xorAst, arg2_eq);
        			}

        			expr_ref implyL(ctx.mk_eq_atom(concat, str), m);
        			expr_ref implyR1(m);
        			if (xor_pos == 0) {
        				// negate
        				expr_ref concat_eq_str(ctx.mk_eq_atom(concat, str), m);
        				expr_ref negate_ast(m.mk_not(concat_eq_str), m);
        				assert_axiom(negate_ast);
        			} else {
        				if (xor_pos == 1) {
        				    and_items[0] = xor_items[0];
        				    implyR1 = m.mk_and(and_count, and_items);
        				} else {
        				    and_items[0] = m.mk_or(xor_pos, xor_items);
        				    implyR1 = m.mk_and(and_count, and_items);
        				}
        				assert_implication(implyL, implyR1);
        			}
        		} /* (arg1Len != 1 || arg2Len != 1) */
        	} /* if (Concat(arg1, arg2) == NULL) */
        }
    }
}

void theory_str::more_len_tests(expr * lenTester, std::string lenTesterValue) {
    ast_manager & m = get_manager();
    if (lenTester_fvar_map.find(lenTester) != lenTester_fvar_map.end()) {
        expr * fVar = lenTester_fvar_map[lenTester];
        expr * toAssert = gen_len_val_options_for_free_var(fVar, lenTester, lenTesterValue);
        TRACE("t_str_detail", tout << "asserting more length tests for free variable " << mk_ismt2_pp(fVar, m) << std::endl;);
        if (toAssert != NULL) {
            assert_axiom(toAssert);
        }
    }
}

void theory_str::more_value_tests(expr * valTester, std::string valTesterValue) {
    ast_manager & m = get_manager();

    expr * fVar = valueTester_fvar_map[valTester];
    int lenTesterCount = fvar_lenTester_map[fVar].size();

    expr * effectiveLenInd = NULL;
    std::string effectiveLenIndiStr = "";
    for (int i = 0; i < lenTesterCount; ++i) {
        expr * len_indicator_pre = fvar_lenTester_map[fVar][i];
        bool indicatorHasEqcValue = false;
        expr * len_indicator_value = get_eqc_value(len_indicator_pre, indicatorHasEqcValue);
        if (indicatorHasEqcValue) {
            std::string len_pIndiStr = m_strutil.get_string_constant_value(len_indicator_value);
            if (len_pIndiStr != "more") {
                effectiveLenInd = len_indicator_pre;
                effectiveLenIndiStr = len_pIndiStr;
                break;
            }
        }
    }
    expr * valueAssert = gen_free_var_options(fVar, effectiveLenInd, effectiveLenIndiStr, valTester, valTesterValue);
    TRACE("t_str_detail", tout << "asserting more value tests for free variable " << mk_ismt2_pp(fVar, m) << std::endl;);
    if (valueAssert != NULL) {
        assert_axiom(valueAssert);
    }
}

bool theory_str::free_var_attempt(expr * nn1, expr * nn2) {
    ast_manager & m = get_manager();

    if (internal_lenTest_vars.contains(nn1) && m_strutil.is_string(nn2)) {
        TRACE("t_str", tout << "acting on equivalence between length tester var " << mk_ismt2_pp(nn1, m)
                << " and constant " << mk_ismt2_pp(nn2, m) << std::endl;);
        more_len_tests(nn1, m_strutil.get_string_constant_value(nn2));
        return true;
    } else if (internal_valTest_vars.contains(nn1) && m_strutil.is_string(nn2)) {
        std::string nn2_str = m_strutil.get_string_constant_value(nn2);
        if (nn2_str == "more") {
            TRACE("t_str", tout << "acting on equivalence between value var " << mk_ismt2_pp(nn1, m)
                            << " and constant " << mk_ismt2_pp(nn2, m) << std::endl;);
            more_value_tests(nn1, nn2_str);
        }
        return true;
    } else if (internal_unrollTest_vars.contains(nn1)) {
    	return true;
    } else {
        return false;
    }
}

void theory_str::handle_equality(expr * lhs, expr * rhs) {
    ast_manager & m = get_manager();
    context & ctx = get_context();
    // both terms must be of sort String
    sort * lhs_sort = m.get_sort(lhs);
    sort * rhs_sort = m.get_sort(rhs);
    sort * str_sort = m.mk_sort(get_family_id(), STRING_SORT);

    if (lhs_sort != str_sort || rhs_sort != str_sort) {
        TRACE("t_str_detail", tout << "skip equality: not String sort" << std::endl;);
        return;
    }

    if (free_var_attempt(lhs, rhs) || free_var_attempt(rhs, lhs)) {
        return;
    }

    if (is_concat(to_app(lhs)) && is_concat(to_app(rhs))) {
        bool nn1HasEqcValue = false;
        bool nn2HasEqcValue = false;
        expr * nn1_value = get_eqc_value(lhs, nn1HasEqcValue);
        expr * nn2_value = get_eqc_value(rhs, nn2HasEqcValue);
        if (nn1HasEqcValue && !nn2HasEqcValue) {
            simplify_parent(rhs, nn1_value);
        }
        if (!nn1HasEqcValue && nn2HasEqcValue) {
            simplify_parent(lhs, nn2_value);
        }

        expr * nn1_arg0 = to_app(lhs)->get_arg(0);
        expr * nn1_arg1 = to_app(lhs)->get_arg(1);
        expr * nn2_arg0 = to_app(rhs)->get_arg(0);
        expr * nn2_arg1 = to_app(rhs)->get_arg(1);
        if (nn1_arg0 == nn2_arg0 && in_same_eqc(nn1_arg1, nn2_arg1)) {
            TRACE("t_str_detail", tout << "skip: lhs arg0 == rhs arg0" << std::endl;);
            return;
        }

        if (nn1_arg1 == nn2_arg1 && in_same_eqc(nn1_arg0, nn2_arg0)) {
            TRACE("t_str_detail", tout << "skip: lhs arg1 == rhs arg1" << std::endl;);
            return;
        }
    }

    if (opt_DeferEQCConsistencyCheck) {
        TRACE("t_str_detail", tout << "opt_DeferEQCConsistencyCheck is set; deferring new_eq_check call" << std::endl;);
    } else {
        // newEqCheck() -- check consistency wrt. existing equivalence classes
        if (!new_eq_check(lhs, rhs)) {
            return;
        }
    }

    // BEGIN new_eq_handler() in strTheory

    {
        rational nn1Len, nn2Len;
        bool nn1Len_exists = get_len_value(lhs, nn1Len);
        bool nn2Len_exists = get_len_value(rhs, nn2Len);
        expr * emptyStr = m_strutil.mk_string("");

        if (nn1Len_exists && nn1Len.is_zero()) {
            if (!in_same_eqc(lhs, emptyStr) && rhs != emptyStr) {
                expr_ref eql(ctx.mk_eq_atom(mk_strlen(lhs), mk_int(0)), m);
                expr_ref eqr(ctx.mk_eq_atom(lhs, emptyStr), m);
                expr_ref toAssert(ctx.mk_eq_atom(eql, eqr), m);
                assert_axiom(toAssert);
            }
        }

        if (nn2Len_exists && nn2Len.is_zero()) {
            if (!in_same_eqc(rhs, emptyStr) && lhs != emptyStr) {
                expr_ref eql(ctx.mk_eq_atom(mk_strlen(rhs), mk_int(0)), m);
                expr_ref eqr(ctx.mk_eq_atom(rhs, emptyStr), m);
                expr_ref toAssert(ctx.mk_eq_atom(eql, eqr), m);
                assert_axiom(toAssert);
            }
        }
    }

    // TODO some setup with haveEQLength() which I skip for now, not sure if necessary

    instantiate_str_eq_length_axiom(ctx.get_enode(lhs), ctx.get_enode(rhs));

    // group terms by equivalence class (groupNodeInEqc())

    std::set<expr*> eqc_concat_lhs;
    std::set<expr*> eqc_var_lhs;
    std::set<expr*> eqc_const_lhs;
    group_terms_by_eqc(lhs, eqc_concat_lhs, eqc_var_lhs, eqc_const_lhs);

    std::set<expr*> eqc_concat_rhs;
    std::set<expr*> eqc_var_rhs;
    std::set<expr*> eqc_const_rhs;
    group_terms_by_eqc(rhs, eqc_concat_rhs, eqc_var_rhs, eqc_const_rhs);

    TRACE("t_str_detail",
        tout << "lhs eqc:" << std::endl;
        tout << "Concats:" << std::endl;
        for (std::set<expr*>::iterator it = eqc_concat_lhs.begin(); it != eqc_concat_lhs.end(); ++it) {
            expr * ex = *it;
            tout << mk_ismt2_pp(ex, get_manager()) << std::endl;
        }
        tout << "Variables:" << std::endl;
        for (std::set<expr*>::iterator it = eqc_var_lhs.begin(); it != eqc_var_lhs.end(); ++it) {
            expr * ex = *it;
            tout << mk_ismt2_pp(ex, get_manager()) << std::endl;
        }
        tout << "Constants:" << std::endl;
        for (std::set<expr*>::iterator it = eqc_const_lhs.begin(); it != eqc_const_lhs.end(); ++it) {
            expr * ex = *it;
            tout << mk_ismt2_pp(ex, get_manager()) << std::endl;
        }

        tout << "rhs eqc:" << std::endl;
        tout << "Concats:" << std::endl;
        for (std::set<expr*>::iterator it = eqc_concat_rhs.begin(); it != eqc_concat_rhs.end(); ++it) {
            expr * ex = *it;
            tout << mk_ismt2_pp(ex, get_manager()) << std::endl;
        }
        tout << "Variables:" << std::endl;
        for (std::set<expr*>::iterator it = eqc_var_rhs.begin(); it != eqc_var_rhs.end(); ++it) {
            expr * ex = *it;
            tout << mk_ismt2_pp(ex, get_manager()) << std::endl;
        }
        tout << "Constants:" << std::endl;
        for (std::set<expr*>::iterator it = eqc_const_rhs.begin(); it != eqc_const_rhs.end(); ++it) {
            expr * ex = *it;
            tout << mk_ismt2_pp(ex, get_manager()) << std::endl;
        }
        );

    // step 1: Concat == Concat
    int hasCommon = 0;
    if (eqc_concat_lhs.size() != 0 && eqc_concat_rhs.size() != 0) {
        std::set<expr*>::iterator itor1 = eqc_concat_lhs.begin();
        std::set<expr*>::iterator itor2 = eqc_concat_rhs.begin();
        for (; itor1 != eqc_concat_lhs.end(); itor1++) {
            if (eqc_concat_rhs.find(*itor1) != eqc_concat_rhs.end()) {
                hasCommon = 1;
                break;
            }
        }
        for (; itor2 != eqc_concat_rhs.end(); itor2++) {
            if (eqc_concat_lhs.find(*itor2) != eqc_concat_lhs.end()) {
                hasCommon = 1;
                break;
            }
        }
        if (hasCommon == 0) {
            simplify_concat_equality(*(eqc_concat_lhs.begin()), *(eqc_concat_rhs.begin()));
        }
    }

    // step 2: Concat == Constant

    if (eqc_const_lhs.size() != 0) {
        expr * conStr = *(eqc_const_lhs.begin());
        std::set<expr*>::iterator itor2 = eqc_concat_rhs.begin();
        for (; itor2 != eqc_concat_rhs.end(); itor2++) {
            solve_concat_eq_str(*itor2, conStr);
        }
    } else if (eqc_const_rhs.size() != 0) {
        expr* conStr = *(eqc_const_rhs.begin());
        std::set<expr*>::iterator itor1 = eqc_concat_lhs.begin();
        for (; itor1 != eqc_concat_lhs.end(); itor1++) {
            solve_concat_eq_str(*itor1, conStr);
        }
    }

    // simplify parents wrt. the equivalence class of both sides
    bool nn1HasEqcValue = false;
    bool nn2HasEqcValue = false;
    // we want the Z3str2 eqc check here...
    expr * nn1_value = z3str2_get_eqc_value(lhs, nn1HasEqcValue);
    expr * nn2_value = z3str2_get_eqc_value(rhs, nn2HasEqcValue);
    if (nn1HasEqcValue && !nn2HasEqcValue) {
        simplify_parent(rhs, nn1_value);
    }

    if (!nn1HasEqcValue && nn2HasEqcValue) {
        simplify_parent(lhs, nn2_value);
    }

    expr * nn1EqConst = NULL;
    std::set<expr*> nn1EqUnrollFuncs;
    get_eqc_allUnroll(lhs, nn1EqConst, nn1EqUnrollFuncs);
    expr * nn2EqConst = NULL;
    std::set<expr*> nn2EqUnrollFuncs;
    get_eqc_allUnroll(rhs, nn2EqConst, nn2EqUnrollFuncs);

    if (nn2EqConst != NULL) {
    	for (std::set<expr*>::iterator itor1 = nn1EqUnrollFuncs.begin(); itor1 != nn1EqUnrollFuncs.end(); itor1++) {
    		process_unroll_eq_const_str(*itor1, nn2EqConst);
    	}
    }

    if (nn1EqConst != NULL) {
    	for (std::set<expr*>::iterator itor2 = nn2EqUnrollFuncs.begin(); itor2 != nn2EqUnrollFuncs.end(); itor2++) {
    		process_unroll_eq_const_str(*itor2, nn1EqConst);
    	}
    }

}

void theory_str::set_up_axioms(expr * ex) {
    ast_manager & m = get_manager();
    context & ctx = get_context();

    sort * ex_sort = m.get_sort(ex);
    sort * str_sort = m.mk_sort(get_family_id(), STRING_SORT);
    sort * bool_sort = m.mk_bool_sort();

    family_id m_arith_fid = m.mk_family_id("arith");
    sort * int_sort = m.mk_sort(m_arith_fid, INT_SORT);

    if (ex_sort == str_sort) {
        TRACE("t_str_detail", tout << "setting up axioms for " << mk_ismt2_pp(ex, get_manager()) <<
                ": expr is of sort String" << std::endl;);
        // set up basic string axioms
        enode * n = ctx.get_enode(ex);
        SASSERT(n);
        m_basicstr_axiom_todo.push_back(n);
        TRACE("t_str_axiom_bug", tout << "add " << mk_pp(ex, m) << " to m_basicstr_axiom_todo" << std::endl;);


        if (is_app(ex)) {
            app * ap = to_app(ex);
            if (is_concat(ap)) {
                // if ex is a concat, set up concat axioms later
                m_concat_axiom_todo.push_back(n);
                // we also want to check whether we can eval this concat,
                // in case the rewriter did not totally finish with this term
                m_concat_eval_todo.push_back(n);
            } else if (is_strlen(ap)) {
            	// if the argument is a variable,
            	// keep track of this for later, we'll need it during model gen
            	expr * var = ap->get_arg(0);
            	app * aVar = to_app(var);
            	if (aVar->get_num_args() == 0 && !is_string(aVar)) {
            		input_var_in_len.insert(var);
            	}
            } else if (is_CharAt(ap)) {
                m_axiom_CharAt_todo.push_back(n);
            } else if (is_Substr(ap)) {
                m_axiom_Substr_todo.push_back(n);
            } else if (is_Replace(ap)) {
                m_axiom_Replace_todo.push_back(n);
            } else if (ap->get_num_args() == 0 && !is_string(ap)) {
                // if ex is a variable, add it to our list of variables
                TRACE("t_str_detail", tout << "tracking variable " << mk_ismt2_pp(ap, get_manager()) << std::endl;);
                variable_set.insert(ex);
                ctx.mark_as_relevant(ex);
                // this might help??
                theory_var v = mk_var(n);
                TRACE("t_str_detail", tout << "variable " << mk_ismt2_pp(ap, get_manager()) << " is #" << v << std::endl;);
            }
        }
    } else if (ex_sort == bool_sort) {
        TRACE("t_str_detail", tout << "setting up axioms for " << mk_ismt2_pp(ex, get_manager()) <<
                ": expr is of sort Bool" << std::endl;);
        // set up axioms for boolean terms

        ensure_enode(ex);
        if (ctx.e_internalized(ex)) {
            enode * n = ctx.get_enode(ex);
            SASSERT(n);

            if (is_app(ex)) {
                app * ap = to_app(ex);
                if (is_StartsWith(ap)) {
                    m_axiom_StartsWith_todo.push_back(n);
                } else if (is_EndsWith(ap)) {
                    m_axiom_EndsWith_todo.push_back(n);
                } else if (is_Contains(ap)) {
                    m_axiom_Contains_todo.push_back(n);
                } else if (is_RegexIn(ap)) {
                	m_axiom_RegexIn_todo.push_back(n);
                }
            }
        } else {
            TRACE("t_str_detail", tout << "WARNING: Bool term " << mk_ismt2_pp(ex, get_manager()) << " not internalized. Delaying axiom setup to prevent a crash." << std::endl;);
            ENSURE(!search_started); // infinite loop prevention
            m_delayed_axiom_setup_terms.push_back(ex);
            return;
        }
    } else if (ex_sort == int_sort) {
        TRACE("t_str_detail", tout << "setting up axioms for " << mk_ismt2_pp(ex, get_manager()) <<
                ": expr is of sort Int" << std::endl;);
        // set up axioms for integer terms
        enode * n = ensure_enode(ex);
        SASSERT(n);

        if (is_app(ex)) {
            app * ap = to_app(ex);
            if (is_Indexof(ap)) {
                m_axiom_Indexof_todo.push_back(n);
            } else if (is_Indexof2(ap)) {
                m_axiom_Indexof2_todo.push_back(n);
            } else if (is_LastIndexof(ap)) {
                m_axiom_LastIndexof_todo.push_back(n);
            }
        }
    } else {
        TRACE("t_str_detail", tout << "setting up axioms for " << mk_ismt2_pp(ex, get_manager()) <<
                ": expr is of wrong sort, ignoring" << std::endl;);
    }

    // if expr is an application, recursively inspect all arguments
    if (is_app(ex)) {
        app * term = (app*)ex;
        unsigned num_args = term->get_num_args();
        for (unsigned i = 0; i < num_args; i++) {
            set_up_axioms(term->get_arg(i));
        }
    }
}

void theory_str::init_search_eh() {
    ast_manager & m = get_manager();
    context & ctx = get_context();

    TRACE("t_str_detail",
        tout << "dumping all asserted formulas:" << std::endl;
        unsigned nFormulas = ctx.get_num_asserted_formulas();
        for (unsigned i = 0; i < nFormulas; ++i) {
            expr * ex = ctx.get_asserted_formula(i);
            tout << mk_ismt2_pp(ex, m) << (ctx.is_relevant(ex) ? " (rel)" : " (NOT REL)") << std::endl;
        }
    );
    /*
     * Recursive descent through all asserted formulas to set up axioms.
     * Note that this is just the input structure and not necessarily things
     * that we know to be true or false. We're just doing this to see
     * which terms are explicitly mentioned.
     */
    unsigned nFormulas = ctx.get_num_asserted_formulas();
    for (unsigned i = 0; i < nFormulas; ++i) {
        expr * ex = ctx.get_asserted_formula(i);
        set_up_axioms(ex);
    }

    /*
     * Similar recursive descent, except over all initially assigned terms.
     * This is done to find equalities between terms, etc. that we otherwise
     * might not get a chance to see.
     */

    /*
    expr_ref_vector assignments(m);
    ctx.get_assignments(assignments);
    for (expr_ref_vector::iterator i = assignments.begin(); i != assignments.end(); ++i) {
        expr * ex = *i;
        if (m.is_eq(ex)) {
            TRACE("t_str_detail", tout << "processing assignment " << mk_ismt2_pp(ex, m) <<
                    ": expr is equality" << std::endl;);
            app * eq = (app*)ex;
            SASSERT(eq->get_num_args() == 2);
            expr * lhs = eq->get_arg(0);
            expr * rhs = eq->get_arg(1);

            enode * e_lhs = ctx.get_enode(lhs);
            enode * e_rhs = ctx.get_enode(rhs);
            std::pair<enode*,enode*> eq_pair(e_lhs, e_rhs);
            m_str_eq_todo.push_back(eq_pair);
        } else {
            TRACE("t_str_detail", tout << "processing assignment " << mk_ismt2_pp(ex, m)
                    << ": expr ignored" << std::endl;);
        }
    }
    */

    // this might be cheating but we need to make sure that certain maps are populated
    // before the first call to new_eq_eh()
    propagate();

    TRACE("t_str", tout << "search started" << std::endl;);
    search_started = true;
}

void theory_str::new_eq_eh(theory_var x, theory_var y) {
    //TRACE("t_str_detail", tout << "new eq: v#" << x << " = v#" << y << std::endl;);
    TRACE("t_str", tout << "new eq: " << mk_ismt2_pp(get_enode(x)->get_owner(), get_manager()) << " = " <<
                                  mk_ismt2_pp(get_enode(y)->get_owner(), get_manager()) << std::endl;);
    /*
    if (m_find.find(x) == m_find.find(y)) {
        return;
    }
    */
    handle_equality(get_enode(x)->get_owner(), get_enode(y)->get_owner());

    // replicate Z3str2 behaviour: merge eqc **AFTER** handle_equality
    m_find.merge(x, y);
}

void theory_str::new_diseq_eh(theory_var x, theory_var y) {
    //TRACE("t_str_detail", tout << "new diseq: v#" << x << " != v#" << y << std::endl;);
    TRACE("t_str", tout << "new diseq: " << mk_ismt2_pp(get_enode(x)->get_owner(), get_manager()) << " != " <<
                                  mk_ismt2_pp(get_enode(y)->get_owner(), get_manager()) << std::endl;);
}

void theory_str::relevant_eh(app * n) {
    TRACE("t_str", tout << "relevant: " << mk_ismt2_pp(n, get_manager()) << std::endl;);
}

void theory_str::assign_eh(bool_var v, bool is_true) {
    context & ctx = get_context();
    TRACE("t_str", tout << "assert: v" << v << " #" << ctx.bool_var2expr(v)->get_id() << " is_true: " << is_true << std::endl;);
}

void theory_str::push_scope_eh() {
    theory::push_scope_eh();
    m_trail_stack.push_scope();

    // TODO out-of-scope term debugging, see comment in pop_scope_eh()
    /*
    context & ctx = get_context();
    ast_manager & m = get_manager();
    expr_ref_vector assignments(m);
    ctx.get_assignments(assignments);
    */

    sLevel += 1;
    TRACE("t_str", tout << "push to " << sLevel << std::endl;);
    TRACE_CODE(if (is_trace_enabled("t_str_dump_assign_on_scope_change")) { dump_assignments(); });
}

void theory_str::recursive_check_variable_scope(expr * ex) {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    if (is_app(ex)) {
        app * a = to_app(ex);
        if (a->get_num_args() == 0) {
            // we only care about string variables
            sort * s = m.get_sort(ex);
            sort * string_sort = m.mk_sort(get_family_id(), STRING_SORT);
            if (s != string_sort) {
                return;
            }
            // base case: string constant / var
            if (m_strutil.is_string(a)) {
                return;
            } else {
                // assume var
                if (variable_set.find(ex) == variable_set.end()
                        && internal_variable_set.find(ex) == internal_variable_set.end()) {
                    TRACE("t_str_detail", tout << "WARNING: possible reference to out-of-scope variable " << mk_pp(ex, m) << std::endl;);
                }
            }
        } else {
            for (unsigned i = 0; i < a->get_num_args(); ++i) {
                recursive_check_variable_scope(a->get_arg(i));
            }
        }
    }
}

void theory_str::check_variable_scope() {
    if (!opt_CheckVariableScope) {
        return;
    }

    if (!is_trace_enabled("t_str_detail")) {
    	return;
    }

    TRACE("t_str_detail", tout << "checking scopes of variables in the current assignment" << std::endl;);

    context & ctx = get_context();
    ast_manager & m = get_manager();

    expr_ref_vector assignments(m);
    ctx.get_assignments(assignments);
    for (expr_ref_vector::iterator i = assignments.begin(); i != assignments.end(); ++i) {
        expr * ex = *i;
        recursive_check_variable_scope(ex);
    }
}

void theory_str::pop_scope_eh(unsigned num_scopes) {
    sLevel -= num_scopes;
    TRACE("t_str", tout << "pop " << num_scopes << " to " << sLevel << std::endl;);
    // TODO: figure out what's going out of scope and why
    context & ctx = get_context();
    ast_manager & m = get_manager();
    expr_ref_vector assignments(m);
    ctx.get_assignments(assignments);

    TRACE_CODE(if (is_trace_enabled("t_str_dump_assign_on_scope_change")) { dump_assignments(); });

    // list of expr* to remove from cut_var_map
    ptr_vector<expr> cutvarmap_removes;

    obj_map<expr, std::stack<T_cut *> >::iterator varItor = cut_var_map.begin();
    while (varItor != cut_var_map.end()) {
    	std::stack<T_cut*> & val = cut_var_map[varItor->m_key];
        while ((val.size() > 0) && (val.top()->level != 0) && (val.top()->level >= sLevel)) {
            T_cut * aCut = val.top();
            val.pop();
            // dealloc(aCut); // TODO find a safer way to do this, it is causing a crash
        }
        if (val.size() == 0) {
        	cutvarmap_removes.insert(varItor->m_key);
        }
        varItor++;
    }

    if (!cutvarmap_removes.empty()) {
    	ptr_vector<expr>::iterator it = cutvarmap_removes.begin();
    	for (; it != cutvarmap_removes.end(); ++it) {
    		expr * ex = *it;
    		cut_var_map.remove(ex);
    	}
    }

    /*
    // see if any internal variables went out of scope
    for (int check_level = sLevel + num_scopes ; check_level > sLevel; --check_level) {
        TRACE("t_str_detail", tout << "cleaning up internal variables at scope level " << check_level << std::endl;);
        std::map<int, std::set<expr*> >::iterator it = internal_variable_scope_levels.find(check_level);
        if (it != internal_variable_scope_levels.end()) {
            unsigned count = 0;
            std::set<expr*> vars = it->second;
            for (std::set<expr*>::iterator var_it = vars.begin(); var_it != vars.end(); ++var_it) {
                TRACE("t_str_detail", tout << "clean up variable " << mk_pp(*var_it, get_manager()) << std::endl;);
                variable_set.erase(*var_it);
                internal_variable_set.erase(*var_it);
                regex_variable_set.erase(*var_it);
                internal_unrollTest_vars.erase(*var_it);
                count += 1;
            }
            TRACE("t_str_detail", tout << "cleaned up " << count << " variables" << std::endl;);
            vars.clear();
        }
    }
    */

    // TODO use the trail stack to do this for us! requires lots of refactoring
    // TODO if this works, possibly remove axioms from other vectors as well
    ptr_vector<enode> new_m_basicstr;
    for (ptr_vector<enode>::iterator it = m_basicstr_axiom_todo.begin(); it != m_basicstr_axiom_todo.end(); ++it) {
        enode * e = *it;
        app * a = e->get_owner();
        TRACE("t_str_axiom_bug", tout << "consider deleting " << mk_pp(a, get_manager())
                << ", enode scope level is " << e->get_iscope_lvl()
                << std::endl;);
        if (e->get_iscope_lvl() <= (unsigned)sLevel) {
            new_m_basicstr.push_back(e);
        }
    }
    m_basicstr_axiom_todo.reset();
    m_basicstr_axiom_todo = new_m_basicstr;

    m_trail_stack.pop_scope(num_scopes);
    theory::pop_scope_eh(num_scopes);

    //check_variable_scope();
}

void theory_str::dump_assignments() {
    TRACE_CODE(
        ast_manager & m = get_manager();
        context & ctx = get_context();
        tout << "dumping all assignments:" << std::endl;
        expr_ref_vector assignments(m);
        ctx.get_assignments(assignments);
        for (expr_ref_vector::iterator i = assignments.begin(); i != assignments.end(); ++i) {
            expr * ex = *i;
            tout << mk_ismt2_pp(ex, m) << (ctx.is_relevant(ex) ? "" : " (NOT REL)") << std::endl;
        }
	);
}

void theory_str::classify_ast_by_type(expr * node, std::map<expr*, int> & varMap,
		std::map<expr*, int> & concatMap, std::map<expr*, int> & unrollMap) {

	// check whether the node is a string variable;
	// testing set membership here bypasses several expensive checks.
    // note that internal variables don't count if they're only length tester / value tester vars.
	if (variable_set.find(node) != variable_set.end()
			&& internal_lenTest_vars.find(node) == internal_lenTest_vars.end()
			&& internal_valTest_vars.find(node) == internal_valTest_vars.end()
	        && internal_unrollTest_vars.find(node) == internal_unrollTest_vars.end()) {
	    if (varMap[node] != 1) {
	        TRACE("t_str_detail", tout << "new variable: " << mk_pp(node, get_manager()) << std::endl;);
	    }
		varMap[node] = 1;
	}
	// check whether the node is a function that we want to inspect
	else if (is_app(node)) { // TODO
		app * aNode = to_app(node);
		if (is_strlen(aNode)) {
			// Length
			return;
		} else if (is_concat(aNode)) {
			expr * arg0 = aNode->get_arg(0);
			expr * arg1 = aNode->get_arg(1);
			bool arg0HasEq = false;
			bool arg1HasEq = false;
			expr * arg0Val = get_eqc_value(arg0, arg0HasEq);
			expr * arg1Val = get_eqc_value(arg1, arg1HasEq);

			int canskip = 0;
			if (arg0HasEq && m_strutil.get_string_constant_value(arg0Val).empty()) {
				canskip = 1;
			}
			if (canskip == 0 && arg1HasEq && m_strutil.get_string_constant_value(arg1Val).empty()) {
				canskip = 1;
			}
			if (canskip == 0 && concatMap.find(node) == concatMap.end()) {
				concatMap[node] = 1;
			}
		} else if (is_Unroll(aNode)) {
			// Unroll
			if (unrollMap.find(node) == unrollMap.end()) {
				unrollMap[node] = 1;
			}
		}
		// recursively visit all arguments
		for (unsigned i = 0; i < aNode->get_num_args(); ++i) {
			expr * arg = aNode->get_arg(i);
			classify_ast_by_type(arg, varMap, concatMap, unrollMap);
		}
	}
}

// NOTE: this function used to take an argument `Z3_ast node`;
// it was not used and so was removed from the signature
void theory_str::classify_ast_by_type_in_positive_context(std::map<expr*, int> & varMap,
		std::map<expr*, int> & concatMap, std::map<expr*, int> & unrollMap) {

	context & ctx = get_context();
	ast_manager & m = get_manager();
	expr_ref_vector assignments(m);
	ctx.get_assignments(assignments);

	for (expr_ref_vector::iterator it = assignments.begin(); it != assignments.end(); ++it) {
		expr * argAst = *it;
		// the original code jumped through some hoops to check whether the AST node
		// is a function, then checked whether that function is "interesting".
		// however, the only thing that's considered "interesting" is an equality predicate.
		// so we bypass a huge amount of work by doing the following...

		if (m.is_eq(argAst)) {
		    TRACE("t_str_detail", tout
		            << "eq ast " << mk_pp(argAst, m) << " is between args of sort "
		            << m.get_sort(to_app(argAst)->get_arg(0))->get_name()
		            << std::endl;);
			classify_ast_by_type(argAst, varMap, concatMap, unrollMap);
		}
	}
}

inline expr * theory_str::get_alias_index_ast(std::map<expr*, expr*> & aliasIndexMap, expr * node) {
  if (aliasIndexMap.find(node) != aliasIndexMap.end())
    return aliasIndexMap[node];
  else
    return node;
}

inline expr * theory_str::getMostLeftNodeInConcat(expr * node) {
	app * aNode = to_app(node);
	if (!is_concat(aNode)) {
		return node;
	} else {
		expr * concatArgL = aNode->get_arg(0);
		return getMostLeftNodeInConcat(concatArgL);
	}
}

inline expr * theory_str::getMostRightNodeInConcat(expr * node) {
	app * aNode = to_app(node);
	if (!is_concat(aNode)) {
		return node;
	} else {
		expr * concatArgR = aNode->get_arg(1);
		return getMostRightNodeInConcat(concatArgR);
	}
}

void theory_str::trace_ctx_dep(std::ofstream & tout,
        std::map<expr*, expr*> & aliasIndexMap,
        std::map<expr*, expr*> & var_eq_constStr_map,
        std::map<expr*, std::map<expr*, int> > & var_eq_concat_map,
		std::map<expr*, std::map<expr*, int> > & var_eq_unroll_map,
        std::map<expr*, expr*> & concat_eq_constStr_map,
        std::map<expr*, std::map<expr*, int> > & concat_eq_concat_map,
		std::map<expr*, std::set<expr*> > & unrollGroupMap) {
#ifdef _TRACE
	context & ctx = get_context();
    ast_manager & mgr = get_manager();
    {
        tout << "(0) alias: variables" << std::endl;
        std::map<expr*, std::map<expr*, int> > aliasSumMap;
        std::map<expr*, expr*>::iterator itor0 = aliasIndexMap.begin();
        for (; itor0 != aliasIndexMap.end(); itor0++) {
            aliasSumMap[itor0->second][itor0->first] = 1;
        }
        std::map<expr*, std::map<expr*, int> >::iterator keyItor = aliasSumMap.begin();
        for (; keyItor != aliasSumMap.end(); keyItor++) {
            tout << "    * ";
            tout << mk_pp(keyItor->first, mgr);
            tout << " : ";
            std::map<expr*, int>::iterator innerItor = keyItor->second.begin();
            for (; innerItor != keyItor->second.end(); innerItor++) {
                tout << mk_pp(innerItor->first, mgr);
                tout << ", ";
            }
            tout << std::endl;
        }
        tout << std::endl;
    }

    {
        tout << "(1) var = constStr:" << std::endl;
        std::map<expr*, expr*>::iterator itor1 = var_eq_constStr_map.begin();
        for (; itor1 != var_eq_constStr_map.end(); itor1++) {
            tout << "    * ";
            tout << mk_pp(itor1->first, mgr);
            tout << " = ";
            tout << mk_pp(itor1->second, mgr);
            if (!in_same_eqc(itor1->first, itor1->second)) {
                tout << "   (not true in ctx)";
            }
            tout << std::endl;
        }
        tout << std::endl;
    }

    {
        tout << "(2) var = concat:" << std::endl;
        std::map<expr*, std::map<expr*, int> >::iterator itor2 = var_eq_concat_map.begin();
        for (; itor2 != var_eq_concat_map.end(); itor2++) {
            tout << "    * ";
            tout << mk_pp(itor2->first, mgr);
            tout << " = { ";
            std::map<expr*, int>::iterator i_itor = itor2->second.begin();
            for (; i_itor != itor2->second.end(); i_itor++) {
                tout << mk_pp(i_itor->first, mgr);
                tout << ", ";
            }
            tout << std::endl;
        }
        tout << std::endl;
    }

    {
        tout << "(3) var = unrollFunc:" << std::endl;
        std::map<expr*, std::map<expr*, int> >::iterator itor2 = var_eq_unroll_map.begin();
        for (; itor2 != var_eq_unroll_map.end(); itor2++) {
            tout << "    * " << mk_pp(itor2->first, mgr) << " = { ";
            std::map<expr*, int>::iterator i_itor = itor2->second.begin();
            for (; i_itor != itor2->second.end(); i_itor++) {
            	tout << mk_pp(i_itor->first, mgr) << ", ";
            }
            tout << " }" << std::endl;
        }
        tout << std::endl;
    }

    {
        tout << "(4) concat = constStr:" << std::endl;
        std::map<expr*, expr*>::iterator itor3 = concat_eq_constStr_map.begin();
        for (; itor3 != concat_eq_constStr_map.end(); itor3++) {
            tout << "    * ";
            tout << mk_pp(itor3->first, mgr);
            tout << " = ";
            tout << mk_pp(itor3->second, mgr);
            tout << std::endl;

        }
        tout << std::endl;
    }

    {
        tout << "(5) eq concats:" << std::endl;
        std::map<expr*, std::map<expr*, int> >::iterator itor4 = concat_eq_concat_map.begin();
        for (; itor4 != concat_eq_concat_map.end(); itor4++) {
            if (itor4->second.size() > 1) {
                std::map<expr*, int>::iterator i_itor = itor4->second.begin();
                tout << "    * ";
                for (; i_itor != itor4->second.end(); i_itor++) {
                    tout << mk_pp(i_itor->first, mgr);
                    tout << " , ";
                }
                tout << std::endl;
            }
        }
        tout << std::endl;
    }

    {
        tout << "(6) eq unrolls:" << std::endl;
        std::map<expr*, std::set<expr*> >::iterator itor5 = unrollGroupMap.begin();
        for (; itor5 != unrollGroupMap.end(); itor5++) {
            tout << "    * ";
            std::set<expr*>::iterator i_itor = itor5->second.begin();
            for (; i_itor != itor5->second.end(); i_itor++) {
            	tout << mk_pp(*i_itor, mgr) << ",  ";
            }
            tout << std::endl;
        }
        tout << std::endl;
    }

    {
        tout << "(7) unroll = concats:" << std::endl;
        std::map<expr*, std::set<expr*> >::iterator itor5 = unrollGroupMap.begin();
        for (; itor5 != unrollGroupMap.end(); itor5++) {
            tout << "    * ";
            expr * unroll = itor5->first;
            tout << mk_pp(unroll, mgr) << std::endl;
            enode * e_curr = ctx.get_enode(unroll);
            enode * e_curr_end = e_curr;
            do {
            	app * curr = e_curr->get_owner();
                if (is_concat(curr)) {
                    tout << "      >>> " << mk_pp(curr, mgr) << std::endl;
                }
                e_curr = e_curr->get_next();
            } while (e_curr != e_curr_end);
            tout << std::endl;
        }
        tout << std::endl;
    }
#else
    return;
#endif // _TRACE
}


/*
 * Dependence analysis from current context assignment
 * - "freeVarMap" contains a set of variables that doesn't constrained by Concats.
 *    But it's possible that it's bounded by unrolls
 *    For the case of
 *    (1) var1 = unroll(r1, t1)
 *        var1 is in the freeVarMap
 *        > should unroll r1 for var1
 *    (2) var1 = unroll(r1, t1) /\ var1 = Concat(var2, var3)
 *        var2, var3 are all in freeVar
 *        > should split the unroll function so that var2 and var3 are bounded by new unrolls
 */
int theory_str::ctx_dep_analysis(std::map<expr*, int> & strVarMap, std::map<expr*, int> & freeVarMap,
		std::map<expr*, std::set<expr*> > & unrollGroupMap) {
	std::map<expr*, int> concatMap;
	std::map<expr*, int> unrollMap;
	std::map<expr*, expr*> aliasIndexMap;
	std::map<expr*, expr*> var_eq_constStr_map;
	std::map<expr*, expr*> concat_eq_constStr_map;
	std::map<expr*, std::map<expr*, int> > var_eq_concat_map;
	std::map<expr*, std::map<expr*, int> > var_eq_unroll_map;
	std::map<expr*, std::map<expr*, int> > concat_eq_concat_map;
	std::map<expr*, std::map<expr*, int> > depMap;

	context & ctx = get_context();
	ast_manager & m = get_manager();

	// note that the old API concatenated these assignments into
	// a massive conjunction; we may have the opportunity to avoid that here
	expr_ref_vector assignments(m);
	ctx.get_assignments(assignments);

	// Step 1: get variables / concat AST appearing in the context
	// the thing we iterate over should just be variable_set - internal_variable_set
	// so we avoid computing the set difference (but this might be slower)
	for(std::set<expr*>::iterator it = variable_set.begin(); it != variable_set.end(); ++it) {
		expr* var = *it;
		if (internal_variable_set.find(var) == internal_variable_set.end()) {
		    TRACE("t_str_detail", tout << "new variable: " << mk_pp(var, m) << std::endl;);
		    strVarMap[*it] = 1;
		}
	}
	classify_ast_by_type_in_positive_context(strVarMap, concatMap, unrollMap);

	std::map<expr*, expr*> aliasUnrollSet;
	std::map<expr*, int>::iterator unrollItor = unrollMap.begin();
	for (; unrollItor != unrollMap.end(); ++unrollItor) {
		if (aliasUnrollSet.find(unrollItor->first) != aliasUnrollSet.end()) {
			continue;
		}
		expr * aRoot = NULL;
		enode * e_currEqc = ctx.get_enode(unrollItor->first);
		enode * e_curr = e_currEqc;
		do {
			app * curr = e_currEqc->get_owner();
			if (is_Unroll(curr)) {
				if (aRoot == NULL) {
					aRoot = curr;
				}
				aliasUnrollSet[curr] = aRoot;
			}
			e_currEqc = e_currEqc->get_next();
		} while (e_currEqc != e_curr);
	}

	for (unrollItor = unrollMap.begin(); unrollItor != unrollMap.end(); unrollItor++) {
	    expr * unrFunc = unrollItor->first;
	    expr * urKey = aliasUnrollSet[unrFunc];
	    unrollGroupMap[urKey].insert(unrFunc);
	}

	// Step 2: collect alias relation
	// e.g. suppose we have the equivalence class {x, y, z};
	// then we set aliasIndexMap[y] = x
	// and aliasIndexMap[z] = x

	std::map<expr*, int>::iterator varItor = strVarMap.begin();
	for (; varItor != strVarMap.end(); ++varItor) {
	    if (aliasIndexMap.find(varItor->first) != aliasIndexMap.end()) {
	        continue;
	    }
	    expr * aRoot = NULL;
	    expr * curr = varItor->first;
	    do {
	        if (variable_set.find(curr) != variable_set.end()) { // TODO internal_variable_set?
	            if (aRoot == NULL) {
	                aRoot = curr;
	            } else {
	                aliasIndexMap[curr] = aRoot;
	            }
	        }
	        curr = get_eqc_next(curr);
	    } while (curr != varItor->first);
	}

	// Step 3: Collect interested cases

	varItor = strVarMap.begin();
	for (; varItor != strVarMap.end(); ++varItor) {
	    expr * deAliasNode = get_alias_index_ast(aliasIndexMap, varItor->first);
	    // Case 1: variable = string constant
	    // e.g. z = "str1" ::= var_eq_constStr_map[z] = "str1"

	    if (var_eq_constStr_map.find(deAliasNode) == var_eq_constStr_map.end()) {
	        bool nodeHasEqcValue = false;
	        expr * nodeValue = get_eqc_value(deAliasNode, nodeHasEqcValue);
	        if (nodeHasEqcValue) {
	            var_eq_constStr_map[deAliasNode] = nodeValue;
	        }
	    }

	    // Case 2: var_eq_concat
	    // e.g. z = concat("str1", b) ::= var_eq_concat[z][concat(c, "str2")] = 1
	    // var_eq_unroll
	    // e.g. z = unroll(...) ::= var_eq_unroll[z][unroll(...)] = 1

	    if (var_eq_concat_map.find(deAliasNode) == var_eq_concat_map.end()) {
	        expr * curr = get_eqc_next(deAliasNode);
	        while (curr != deAliasNode) {
	            app * aCurr = to_app(curr);
	            // collect concat
	            if (is_concat(aCurr)) {
	                expr * arg0 = aCurr->get_arg(0);
	                expr * arg1 = aCurr->get_arg(1);
	                bool arg0HasEqcValue = false;
	                bool arg1HasEqcValue = false;
	                expr * arg0_value = get_eqc_value(arg0, arg0HasEqcValue);
	                expr * arg1_value = get_eqc_value(arg1, arg1HasEqcValue);

	                bool is_arg0_emptyStr = false;
	                if (arg0HasEqcValue) {
	                    const char * strval = 0;
	                    m_strutil.is_string(arg0_value, &strval);
	                    if (strcmp(strval, "") == 0) {
	                        is_arg0_emptyStr = true;
	                    }
	                }

	                bool is_arg1_emptyStr = false;
	                if (arg1HasEqcValue) {
	                    const char * strval = 0;
	                    m_strutil.is_string(arg1_value, &strval);
	                    if (strcmp(strval, "") == 0) {
	                        is_arg1_emptyStr = true;
	                    }
	                }

	                if (!is_arg0_emptyStr && !is_arg1_emptyStr) {
	                    var_eq_concat_map[deAliasNode][curr] = 1;
	                }
	            } else if (is_Unroll(to_app(curr))) {
	                var_eq_unroll_map[deAliasNode][curr] = 1;
	            }

	            curr = get_eqc_next(curr);
	        }
	    }

	} // for(varItor in strVarMap)

	// --------------------------------------------------
	// * collect aliasing relation among eq concats
	//   e.g EQC={concat1, concat2, concat3}
	//       concats_eq_Index_map[concat2] = concat1
	//       concats_eq_Index_map[concat3] = concat1
	// --------------------------------------------------

	std::map<expr*, expr*> concats_eq_index_map;
	std::map<expr*, int>::iterator concatItor = concatMap.begin();
	for(; concatItor != concatMap.end(); ++concatItor) {
		if (concats_eq_index_map.find(concatItor->first) != concats_eq_index_map.end()) {
			continue;
		}
		expr * aRoot = NULL;
		expr * curr = concatItor->first;
		do {
			if (is_concat(to_app(curr))) {
				if (aRoot == NULL) {
					aRoot = curr;
				} else {
					concats_eq_index_map[curr] = aRoot;
				}
			}
			curr = get_eqc_next(curr);
		} while (curr != concatItor->first);
	}

	concatItor = concatMap.begin();
	for(; concatItor != concatMap.end(); ++concatItor) {
		expr * deAliasConcat = NULL;
		if (concats_eq_index_map.find(concatItor->first) != concats_eq_index_map.end()) {
			deAliasConcat = concats_eq_index_map[concatItor->first];
		} else {
			deAliasConcat = concatItor->first;
		}

		// (3) concat_eq_conststr, e.g. concat(a,b) = "str1"
		if (concat_eq_constStr_map.find(deAliasConcat) == concat_eq_constStr_map.end()) {
			bool nodeHasEqcValue = false;
			expr * nodeValue = get_eqc_value(deAliasConcat, nodeHasEqcValue);
			if (nodeHasEqcValue) {
				concat_eq_constStr_map[deAliasConcat] = nodeValue;
			}
		}

		// (4) concat_eq_concat, e.g.
		// concat(a,b) = concat("str1", c) AND z = concat(a,b) AND z = concat(e,f)
		if (concat_eq_concat_map.find(deAliasConcat) == concat_eq_concat_map.end()) {
			expr * curr = deAliasConcat;
			do {
				if (is_concat(to_app(curr))) {
					// curr cannot be reduced
					if (concatMap.find(curr) != concatMap.end()) {
						concat_eq_concat_map[deAliasConcat][curr] = 1;
					}
				}
				curr = get_eqc_next(curr);
			} while (curr != deAliasConcat);
		}
	}

	// print some debugging info
	TRACE("t_str_detail", trace_ctx_dep(tout, aliasIndexMap, var_eq_constStr_map,
	        var_eq_concat_map, var_eq_unroll_map,
			concat_eq_constStr_map, concat_eq_concat_map, unrollGroupMap););

	if (!contain_pair_bool_map.empty()) {
		compute_contains(aliasIndexMap, concats_eq_index_map, var_eq_constStr_map, concat_eq_constStr_map, var_eq_concat_map);
	}

	// step 4: dependence analysis

	// (1) var = string constant
	for (std::map<expr*, expr*>::iterator itor = var_eq_constStr_map.begin();
			itor != var_eq_constStr_map.end(); ++itor) {
		expr * var = get_alias_index_ast(aliasIndexMap, itor->first);
		expr * strAst = itor->second;
		depMap[var][strAst] = 1;
	}

	// (2) var = concat
	for (std::map<expr*, std::map<expr*, int> >::iterator itor = var_eq_concat_map.begin();
			itor != var_eq_concat_map.end(); ++itor) {
		expr * var = get_alias_index_ast(aliasIndexMap, itor->first);
		for (std::map<expr*, int>::iterator itor1 = itor->second.begin(); itor1 != itor->second.end(); ++itor1) {
			expr * concat = itor1->first;
			std::map<expr*, int> inVarMap;
			std::map<expr*, int> inConcatMap;
			std::map<expr*, int> inUnrollMap;
			classify_ast_by_type(concat, inVarMap, inConcatMap, inUnrollMap);
			for (std::map<expr*, int>::iterator itor2 = inVarMap.begin(); itor2 != inVarMap.end(); ++itor2) {
				expr * varInConcat = get_alias_index_ast(aliasIndexMap, itor2->first);
				if (!(depMap[var].find(varInConcat) != depMap[var].end() && depMap[var][varInConcat] == 1)) {
					depMap[var][varInConcat] = 2;
				}
			}
		}
	}

	for (std::map<expr*, std::map<expr*, int> >::iterator itor = var_eq_unroll_map.begin();
		itor != var_eq_unroll_map.end(); itor++) {
		expr * var = get_alias_index_ast(aliasIndexMap, itor->first);
		for (std::map<expr*, int>::iterator itor1 = itor->second.begin(); itor1 != itor->second.end(); itor1++) {
			expr * unrollFunc = itor1->first;
			std::map<expr*, int> inVarMap;
			std::map<expr*, int> inConcatMap;
			std::map<expr*, int> inUnrollMap;
			classify_ast_by_type(unrollFunc, inVarMap, inConcatMap, inUnrollMap);
			for (std::map<expr*, int>::iterator itor2 = inVarMap.begin(); itor2 != inVarMap.end(); itor2++) {
				expr * varInFunc = get_alias_index_ast(aliasIndexMap, itor2->first);

				TRACE("t_str_detail", tout << "var in unroll = " <<
						mk_ismt2_pp(itor2->first, m) << std::endl
						<< "dealiased var = " << mk_ismt2_pp(varInFunc, m) << std::endl;);

				// it's possible that we have both (Unroll $$_regVar_0 $$_unr_0) /\ (Unroll abcd $$_unr_0),
				// while $$_regVar_0 = "abcd"
				// have to exclude such cases
				bool varHasValue = false;
				get_eqc_value(varInFunc, varHasValue);
				if (varHasValue)
					continue;

				if (depMap[var].find(varInFunc) == depMap[var].end()) {
					depMap[var][varInFunc] = 6;
				}
			}
		}
	}

	// (3) concat = string constant
	for (std::map<expr*, expr*>::iterator itor = concat_eq_constStr_map.begin();
			itor != concat_eq_constStr_map.end(); itor++) {
		expr * concatAst = itor->first;
		expr * constStr = itor->second;
		std::map<expr*, int> inVarMap;
		std::map<expr*, int> inConcatMap;
		std::map<expr*, int> inUnrollMap;
		classify_ast_by_type(concatAst, inVarMap, inConcatMap, inUnrollMap);
		for (std::map<expr*, int>::iterator itor2 = inVarMap.begin(); itor2 != inVarMap.end(); itor2++) {
			expr * varInConcat = get_alias_index_ast(aliasIndexMap, itor2->first);
			if (!(depMap[varInConcat].find(constStr) != depMap[varInConcat].end() && depMap[varInConcat][constStr] == 1))
				depMap[varInConcat][constStr] = 3;
		}
	}

	// (4) equivalent concats
	//     - possibility 1 : concat("str", v1) = concat(concat(v2, v3), v4) = concat(v5, v6)
	//         ==> v2, v5 are constrained by "str"
	//     - possibility 2 : concat(v1, "str") = concat(v2, v3) = concat(v4, v5)
	//         ==> v2, v4 are constrained by "str"
	//--------------------------------------------------------------

	std::map<expr*, expr*> mostLeftNodes;
	std::map<expr*, expr*> mostRightNodes;

	std::map<expr*, int> mLIdxMap;
	std::map<int, std::set<expr*> > mLMap;
	std::map<expr*, int> mRIdxMap;
	std::map<int, std::set<expr*> > mRMap;
	std::set<expr*> nSet;

	for (std::map<expr*, std::map<expr*, int> >::iterator itor = concat_eq_concat_map.begin();
			itor != concat_eq_concat_map.end(); itor++) {
		mostLeftNodes.clear();
		mostRightNodes.clear();

		expr * mLConst = NULL;
		expr * mRConst = NULL;

		for (std::map<expr*, int>::iterator itor1 = itor->second.begin(); itor1 != itor->second.end(); itor1++) {
			expr * concatNode = itor1->first;
			expr * mLNode = getMostLeftNodeInConcat(concatNode);
			const char * strval;
			if (m_strutil.is_string(to_app(mLNode), & strval)) {
				if (mLConst == NULL && strcmp(strval, "") != 0) {
					mLConst = mLNode;
				}
			} else {
				mostLeftNodes[mLNode] = concatNode;
			}

			expr * mRNode = getMostRightNodeInConcat(concatNode);
			if (m_strutil.is_string(to_app(mRNode), & strval)) {
				if (mRConst == NULL && strcmp(strval, "") != 0) {
					mRConst = mRNode;
				}
			} else {
				mostRightNodes[mRNode] = concatNode;
			}
		}

		if (mLConst != NULL) {
			// -------------------------------------------------------------------------------------
			// The left most variable in a concat is constrained by a constant string in eqc concat
			// -------------------------------------------------------------------------------------
			// e.g. Concat(x, ...) = Concat("abc", ...)
			// -------------------------------------------------------------------------------------
			for (std::map<expr*, expr*>::iterator itor1 = mostLeftNodes.begin();
					itor1 != mostLeftNodes.end(); itor1++) {
				expr * deVar = get_alias_index_ast(aliasIndexMap, itor1->first);
				if (depMap[deVar].find(mLConst) == depMap[deVar].end() || depMap[deVar][mLConst] != 1) {
					depMap[deVar][mLConst] = 4;
				}
			}
		}

		{
			// -------------------------------------------------------------------------------------
			// The left most variables in eqc concats are constrained by each other
			// -------------------------------------------------------------------------------------
			// e.g. concat(x, ...) = concat(u, ...) = ...
			//      x and u are constrained by each other
			// -------------------------------------------------------------------------------------
			nSet.clear();
			std::map<expr*, expr*>::iterator itl = mostLeftNodes.begin();
			for (; itl != mostLeftNodes.end(); itl++) {
				bool lfHasEqcValue = false;
				get_eqc_value(itl->first, lfHasEqcValue);
				if (lfHasEqcValue)
					continue;
				expr * deVar = get_alias_index_ast(aliasIndexMap, itl->first);
				nSet.insert(deVar);
			}

			if (nSet.size() > 1) {
				int lId = -1;
				for (std::set<expr*>::iterator itor2 = nSet.begin(); itor2 != nSet.end(); itor2++) {
					if (mLIdxMap.find(*itor2) != mLIdxMap.end()) {
						lId = mLIdxMap[*itor2];
						break;
					}
				}
				if (lId == -1)
					lId = mLMap.size();
				for (std::set<expr*>::iterator itor2 = nSet.begin(); itor2 != nSet.end(); itor2++) {
					bool itorHasEqcValue = false;
					get_eqc_value(*itor2, itorHasEqcValue);
					if (itorHasEqcValue)
						continue;
					mLIdxMap[*itor2] = lId;
					mLMap[lId].insert(*itor2);
				}
			}
		}

		if (mRConst != NULL) {
			for (std::map<expr*, expr*>::iterator itor1 = mostRightNodes.begin();
					itor1 != mostRightNodes.end(); itor1++) {
				expr * deVar = get_alias_index_ast(aliasIndexMap, itor1->first);
				if (depMap[deVar].find(mRConst) == depMap[deVar].end() || depMap[deVar][mRConst] != 1) {
					depMap[deVar][mRConst] = 5;
				}
			}
		}

		{
			nSet.clear();
			std::map<expr*, expr*>::iterator itr = mostRightNodes.begin();
			for (; itr != mostRightNodes.end(); itr++) {
				expr * deVar = get_alias_index_ast(aliasIndexMap, itr->first);
				nSet.insert(deVar);
			}
			if (nSet.size() > 1) {
				int rId = -1;
				std::set<expr*>::iterator itor2 = nSet.begin();
				for (; itor2 != nSet.end(); itor2++) {
					if (mRIdxMap.find(*itor2) != mRIdxMap.end()) {
						rId = mRIdxMap[*itor2];
						break;
					}
				}
				if (rId == -1)
					rId = mRMap.size();
				for (itor2 = nSet.begin(); itor2 != nSet.end(); itor2++) {
					bool rHasEqcValue = false;
					get_eqc_value(*itor2, rHasEqcValue);
					if (rHasEqcValue)
						continue;
					mRIdxMap[*itor2] = rId;
					mRMap[rId].insert(*itor2);
				}
			}
		}
	}

	// print the dependence map
	TRACE("t_str_detail",
    tout << "Dependence Map" << std::endl;
	for(std::map<expr*, std::map<expr*, int> >::iterator itor = depMap.begin(); itor != depMap.end(); itor++) {
	    tout << mk_pp(itor->first, m);
	    rational nnLen;
	    bool nnLen_exists = get_len_value(itor->first, nnLen);
	    tout << "  [len = " << (nnLen_exists ? nnLen.to_string() : "?") << "] \t-->\t";
	    for (std::map<expr*, int>::iterator itor1 = itor->second.begin(); itor1 != itor->second.end(); itor1++) {
	        tout << mk_pp(itor1->first, m) << "(" << itor1->second << "), ";
	    }
	    tout << std::endl;
	}
	        );

	// step, errr, 5: compute free variables based on the dependence map

	// the case dependence map is empty, every var in VarMap is free
	//---------------------------------------------------------------
	// remove L/R most var in eq concat since they are constrained with each other
	std::map<expr*, std::map<expr*, int> > lrConstrainedMap;
	for (std::map<int, std::set<expr*> >::iterator itor = mLMap.begin(); itor != mLMap.end(); itor++) {
		for (std::set<expr*>::iterator it1 = itor->second.begin(); it1 != itor->second.end(); it1++) {
			std::set<expr*>::iterator it2 = it1;
			it2++;
			for (; it2 != itor->second.end(); it2++) {
				expr * n1 = *it1;
				expr * n2 = *it2;
				lrConstrainedMap[n1][n2] = 1;
				lrConstrainedMap[n2][n1] = 1;
			}
		}
	}
	for (std::map<int, std::set<expr*> >::iterator itor = mRMap.begin(); itor != mRMap.end(); itor++) {
		for (std::set<expr*>::iterator it1 = itor->second.begin(); it1 != itor->second.end(); it1++) {
			std::set<expr*>::iterator it2 = it1;
			it2++;
			for (; it2 != itor->second.end(); it2++) {
				expr * n1 = *it1;
				expr * n2 = *it2;
				lrConstrainedMap[n1][n2] = 1;
				lrConstrainedMap[n2][n1] = 1;
			}
		}
	}

	if (depMap.size() == 0) {
		std::map<expr*, int>::iterator itor = strVarMap.begin();
		for (; itor != strVarMap.end(); itor++) {
			expr * var = get_alias_index_ast(aliasIndexMap, itor->first);
			if (lrConstrainedMap.find(var) == lrConstrainedMap.end()) {
				freeVarMap[var] = 1;
			} else {
				int lrConstainted = 0;
				std::map<expr*, int>::iterator lrit = freeVarMap.begin();
				for (; lrit != freeVarMap.end(); lrit++) {
					if (lrConstrainedMap[var].find(lrit->first) != lrConstrainedMap[var].end()) {
						lrConstainted = 1;
						break;
					}
				}
				if (lrConstainted == 0) {
					freeVarMap[var] = 1;
				}
			}
		}
	} else {
		// if the keys in aliasIndexMap are not contained in keys in depMap, they are free
		// e.g.,  x= y /\ x = z /\ t = "abc"
		//        aliasIndexMap[y]= x, aliasIndexMap[z] = x
		//        depMap        t ~ "abc"(1)
		//        x should be free
		std::map<expr*, int>::iterator itor2 = strVarMap.begin();
		for (; itor2 != strVarMap.end(); itor2++) {
			if (aliasIndexMap.find(itor2->first) != aliasIndexMap.end()) {
				expr * var = aliasIndexMap[itor2->first];
				if (depMap.find(var) == depMap.end()) {
					if (lrConstrainedMap.find(var) == lrConstrainedMap.end()) {
						freeVarMap[var] = 1;
					} else {
						int lrConstainted = 0;
						std::map<expr*, int>::iterator lrit = freeVarMap.begin();
						for (; lrit != freeVarMap.end(); lrit++) {
							if (lrConstrainedMap[var].find(lrit->first) != lrConstrainedMap[var].end()) {
								lrConstainted = 1;
								break;
							}
						}
						if (lrConstainted == 0) {
							freeVarMap[var] = 1;
						}
					}
				}
			} else if (aliasIndexMap.find(itor2->first) == aliasIndexMap.end()) {
				// if a variable is not in aliasIndexMap and not in depMap, it's free
				if (depMap.find(itor2->first) == depMap.end()) {
					expr * var = itor2->first;
					if (lrConstrainedMap.find(var) == lrConstrainedMap.end()) {
						freeVarMap[var] = 1;
					} else {
						int lrConstainted = 0;
						std::map<expr*, int>::iterator lrit = freeVarMap.begin();
						for (; lrit != freeVarMap.end(); lrit++) {
							if (lrConstrainedMap[var].find(lrit->first) != lrConstrainedMap[var].end()) {
								lrConstainted = 1;
								break;
							}
						}
						if (lrConstainted == 0) {
							freeVarMap[var] = 1;
						}
					}
				}
			}
		}

		std::map<expr*, std::map<expr*, int> >::iterator itor = depMap.begin();
		for (; itor != depMap.end(); itor++) {
			for (std::map<expr*, int>::iterator itor1 = itor->second.begin(); itor1 != itor->second.end(); itor1++) {
				if (variable_set.find(itor1->first) != variable_set.end()) { // expr type = var
					expr * var = get_alias_index_ast(aliasIndexMap, itor1->first);
					// if a var is dep on itself and all dependence are type 2, it's a free variable
					// e.g {y --> x(2), y(2), m --> m(2), n(2)} y,m are free
					{
						if (depMap.find(var) == depMap.end()) {
							if (freeVarMap.find(var) == freeVarMap.end()) {
								if (lrConstrainedMap.find(var) == lrConstrainedMap.end()) {
									freeVarMap[var] = 1;
								} else {
									int lrConstainted = 0;
									std::map<expr*, int>::iterator lrit = freeVarMap.begin();
									for (; lrit != freeVarMap.end(); lrit++) {
										if (lrConstrainedMap[var].find(lrit->first) != lrConstrainedMap[var].end()) {
											lrConstainted = 1;
											break;
										}
									}
									if (lrConstainted == 0) {
										freeVarMap[var] = 1;
									}
								}

							} else {
								freeVarMap[var] = freeVarMap[var] + 1;
							}
						}
					}
				}
			}
		}
	}

	return 0;
}

final_check_status theory_str::final_check_eh() {
    context & ctx = get_context();
    ast_manager & m = get_manager();

    // TODO out-of-scope term debugging, see comment in pop_scope_eh()
    expr_ref_vector assignments(m);
    ctx.get_assignments(assignments);

    if (opt_VerifyFinalCheckProgress) {
        finalCheckProgressIndicator = false;
    }

    TRACE("t_str", tout << "final check" << std::endl;);
    TRACE_CODE(if (is_trace_enabled("t_str_dump_assign")) { dump_assignments(); });
    check_variable_scope();

    if (opt_DeferEQCConsistencyCheck) {
        TRACE("t_str_detail", tout << "performing deferred EQC consistency check" << std::endl;);
        std::set<enode*> eqc_roots;
        for (ptr_vector<enode>::const_iterator it = ctx.begin_enodes(); it != ctx.end_enodes(); ++it) {
            enode * e = *it;
            enode * root = e->get_root();
            eqc_roots.insert(root);
        }

        bool found_inconsistency = false;

        for (std::set<enode*>::iterator it = eqc_roots.begin(); it != eqc_roots.end(); ++it) {
            enode * e = *it;
            app * a = e->get_owner();
            if (!(is_sort_of(m.get_sort(a), m_strutil.get_fid(), STRING_SORT))) {
                TRACE("t_str_detail", tout << "EQC root " << mk_pp(a, m) << " not a string term; skipping" << std::endl;);
            } else {
                TRACE("t_str_detail", tout << "EQC root " << mk_pp(a, m) << " is a string term. Checking this EQC" << std::endl;);
                // first call check_concat_len_in_eqc() on each member of the eqc
                enode * e_it = e;
                enode * e_root = e_it;
                do {
                    bool status = check_concat_len_in_eqc(e_it->get_owner());
                    if (!status) {
                        TRACE("t_str_detail", tout << "concat-len check asserted an axiom on " << mk_pp(e_it->get_owner(), m) << std::endl;);
                        found_inconsistency = true;
                    }
                    e_it = e_it->get_next();
                } while (e_it != e_root);

                // now grab any two distinct elements from the EQC and call new_eq_check() on them
                enode * e1 = e;
                enode * e2 = e1->get_next();
                if (e1 != e2) {
                    TRACE("t_str_detail", tout << "deferred new_eq_check() over EQC of " << mk_pp(e1->get_owner(), m) << " and " << mk_pp(e2->get_owner(), m) << std::endl;);
                    bool result = new_eq_check(e1->get_owner(), e2->get_owner());
                    if (!result) {
                        TRACE("t_str_detail", tout << "new_eq_check found inconsistencies" << std::endl;);
                        found_inconsistency = true;
                    }
                }
            }
        }

        if (found_inconsistency) {
            TRACE("t_str", tout << "Found inconsistency in final check! Returning to search." << std::endl;);
            return FC_CONTINUE;
        } else {
            TRACE("t_str", tout << "Deferred consistency check passed. Continuing in final check." << std::endl;);
        }
    }

    // run dependence analysis to find free string variables
    std::map<expr*, int> varAppearInAssign;
    std::map<expr*, int> freeVar_map;
    std::map<expr*, std::set<expr*> > unrollGroup_map;
    int conflictInDep = ctx_dep_analysis(varAppearInAssign, freeVar_map, unrollGroup_map);
    if (conflictInDep == -1) {
    	// return Z3_TRUE;
    	return FC_DONE;
    }

    // Check every variable to see if it's eq. to some string constant.
    // If not, mark it as free.
    bool needToAssignFreeVars = false;
    std::set<expr*> free_variables;
    std::set<expr*> unused_internal_variables;
    TRACE("t_str_detail", tout << variable_set.size() << " variables in variable_set" << std::endl;);
    for (std::set<expr*>::iterator it = variable_set.begin(); it != variable_set.end(); ++it) {
        TRACE("t_str_detail", tout << "checking eqc of variable " << mk_ismt2_pp(*it, m) << std::endl;);
        bool has_eqc_value = false;
        get_eqc_value(*it, has_eqc_value);
        if (!has_eqc_value) {
            // if this is an internal variable, it can be ignored...I think
            if (internal_variable_set.find(*it) != internal_variable_set.end() || regex_variable_set.find(*it) != regex_variable_set.end()) {
                TRACE("t_str_detail", tout << "WARNING: free internal variable " << mk_ismt2_pp(*it, m) << std::endl;);
                //unused_internal_variables.insert(*it);
            } else {
                needToAssignFreeVars = true;
                free_variables.insert(*it);
            }
        }
    }

    if (!needToAssignFreeVars) {
        if (unused_internal_variables.empty()) {
            TRACE("t_str", tout << "All variables are assigned. Done!" << std::endl;);
            return FC_DONE;
        } else {
            TRACE("t_str", tout << "Assigning decoy values to free internal variables." << std::endl;);
            for (std::set<expr*>::iterator it = unused_internal_variables.begin(); it != unused_internal_variables.end(); ++it) {
                expr * var = *it;
                expr_ref assignment(m.mk_eq(var, m_strutil.mk_string("**unused**")), m);
                assert_axiom(assignment);
            }
            return FC_CONTINUE;
        }
    }

    CTRACE("t_str", needToAssignFreeVars,
        tout << "Need to assign values to the following free variables:" << std::endl;
        for (std::set<expr*>::iterator itx = free_variables.begin(); itx != free_variables.end(); ++itx) {
            tout << mk_ismt2_pp(*itx, m) << std::endl;
        }
        tout << "freeVar_map has the following entries:" << std::endl;
        for (std::map<expr*, int>::iterator fvIt = freeVar_map.begin(); fvIt != freeVar_map.end(); fvIt++) {
            expr * var = fvIt->first;
            tout << mk_ismt2_pp(var, m) << std::endl;
        }
    );

    // -----------------------------------------------------------
    // variables in freeVar are those not bounded by Concats
    // classify variables in freeVarMap:
    // (1) freeVar = unroll(r1, t1)
    // (2) vars are not bounded by either concat or unroll
    // -----------------------------------------------------------
    std::map<expr*, std::set<expr*> > fv_unrolls_map;
    std::set<expr*> tmpSet;
    expr * constValue = NULL;
    for (std::map<expr*, int>::iterator fvIt2 = freeVar_map.begin(); fvIt2 != freeVar_map.end(); fvIt2++) {
    	expr * var = fvIt2->first;
    	tmpSet.clear();
    	get_eqc_allUnroll(var, constValue, tmpSet);
    	if (tmpSet.size() > 0) {
    		fv_unrolls_map[var] = tmpSet;
    	}
    }
    // erase var bounded by an unroll function from freeVar_map
    for (std::map<expr*, std::set<expr*> >::iterator fvIt3 = fv_unrolls_map.begin();
    		fvIt3 != fv_unrolls_map.end(); fvIt3++) {
    	expr * var = fvIt3->first;
    	TRACE("t_str_detail", tout << "erase free variable " << mk_pp(var, m) << " from freeVar_map, it is bounded by an Unroll" << std::endl;);
    	freeVar_map.erase(var);
    }

    // collect the case:
    //   * Concat(X, Y) = unroll(r1, t1) /\ Concat(X, Y) = unroll(r2, t2)
    //     concatEqUnrollsMap[Concat(X, Y)] = {unroll(r1, t1), unroll(r2, t2)}

    std::map<expr*, std::set<expr*> > concatEqUnrollsMap;
    for (std::map<expr*, std::set<expr*> >::iterator urItor = unrollGroup_map.begin();
    		urItor != unrollGroup_map.end(); urItor++) {
    	expr * unroll = urItor->first;
    	expr * curr = unroll;
    	do {
    		if (is_concat(to_app(curr))) {
    			concatEqUnrollsMap[curr].insert(unroll);
    			concatEqUnrollsMap[curr].insert(unrollGroup_map[unroll].begin(), unrollGroup_map[unroll].end());
    		}
    		enode * e_curr = ctx.get_enode(curr);
    		curr = e_curr->get_next()->get_owner();
    		// curr = get_eqc_next(curr);
    	} while (curr != unroll);
    }

    std::map<expr*, std::set<expr*> > concatFreeArgsEqUnrollsMap;
    std::set<expr*> fvUnrollSet;
    for (std::map<expr*, std::set<expr*> >::iterator concatItor = concatEqUnrollsMap.begin();
    		concatItor != concatEqUnrollsMap.end(); concatItor++) {
    	expr * concat = concatItor->first;
    	expr * concatArg1 = to_app(concat)->get_arg(0);
    	expr * concatArg2 = to_app(concat)->get_arg(1);
    	bool arg1Bounded = false;
    	bool arg2Bounded = false;
    	// arg1
		if (variable_set.find(concatArg1) != variable_set.end()) {
			if (freeVar_map.find(concatArg1) == freeVar_map.end()) {
				arg1Bounded = true;
			} else {
				fvUnrollSet.insert(concatArg1);
			}
		} else if (is_concat(to_app(concatArg1))) {
			if (concatEqUnrollsMap.find(concatArg1) == concatEqUnrollsMap.end()) {
				arg1Bounded = true;
			}
		}
		// arg2
		if (variable_set.find(concatArg2) != variable_set.end()) {
			if (freeVar_map.find(concatArg2) == freeVar_map.end()) {
				arg2Bounded = true;
			} else {
				fvUnrollSet.insert(concatArg2);
			}
		} else if (is_concat(to_app(concatArg2))) {
			if (concatEqUnrollsMap.find(concatArg2) == concatEqUnrollsMap.end()) {
				arg2Bounded = true;
			}
		}
		if (!arg1Bounded && !arg2Bounded) {
			concatFreeArgsEqUnrollsMap[concat].insert(
					concatEqUnrollsMap[concat].begin(),
					concatEqUnrollsMap[concat].end());
		}
    }
    for (std::set<expr*>::iterator vItor = fvUnrollSet.begin(); vItor != fvUnrollSet.end(); vItor++) {
    	TRACE("t_str_detail", tout << "remove " << mk_pp(*vItor, m) << " from freeVar_map" << std::endl;);
    	freeVar_map.erase(*vItor);
    }

    // Assign free variables
    std::set<expr*> fSimpUnroll;

    constValue = NULL;

    {
        TRACE("t_str_detail", tout << "free var map (#" << freeVar_map.size() << "):" << std::endl;
        for (std::map<expr*, int>::iterator freeVarItor1 = freeVar_map.begin(); freeVarItor1 != freeVar_map.end(); freeVarItor1++) {
            expr * freeVar = freeVarItor1->first;
            rational lenValue;
            bool lenValue_exists = get_len_value(freeVar, lenValue);
            // TODO get_bound_strlen()
            tout << mk_pp(freeVar, m) << " [depCnt = " << freeVarItor1->second << ", length = "
                    << (lenValue_exists ? lenValue.to_string() : "?")
                    << "]" << std::endl;
        }
        );
    }

    for (std::map<expr*, std::set<expr*> >::iterator fvIt2 = concatFreeArgsEqUnrollsMap.begin();
    		fvIt2 != concatFreeArgsEqUnrollsMap.end(); fvIt2++) {
    	expr * concat = fvIt2->first;
    	for (std::set<expr*>::iterator urItor = fvIt2->second.begin(); urItor != fvIt2->second.end(); urItor++) {
    		expr * unroll = *urItor;
    		process_concat_eq_unroll(concat, unroll);
    	}
    }

    // --------
    // experimental free variable assignment - begin
    //   * special handling for variables that are not used in concat
    // --------
    bool testAssign = true;
    if (!testAssign) {
    	for (std::map<expr*, int>::iterator fvIt = freeVar_map.begin(); fvIt != freeVar_map.end(); fvIt++) {
    		expr * freeVar = fvIt->first;
    		/*
    		std::string vName = std::string(Z3_ast_to_string(ctx, freeVar));
    		if (vName.length() >= 9 && vName.substr(0, 9) == "$$_regVar") {
    			continue;
    		}
    		*/
    		// TODO if this variable represents a regular expression, continue
    		expr * toAssert = gen_len_val_options_for_free_var(freeVar, NULL, "");
    		if (toAssert != NULL) {
    			assert_axiom(toAssert);
    		}
    	}
    } else {
    	process_free_var(freeVar_map);
    }
    // experimental free variable assignment - end

    // now deal with removed free variables that are bounded by an unroll
    TRACE("t_str", tout << "fv_unrolls_map (#" << fv_unrolls_map.size() << "):" << std::endl;);
    for (std::map<expr*, std::set<expr*> >::iterator fvIt1 = fv_unrolls_map.begin();
    		fvIt1 != fv_unrolls_map.end(); fvIt1++) {
    	expr * var = fvIt1->first;
    	fSimpUnroll.clear();
    	get_eqc_simpleUnroll(var, constValue, fSimpUnroll);
    	if (fSimpUnroll.size() == 0) {
    		gen_assign_unroll_reg(fv_unrolls_map[var]);
    	} else {
    		expr * toAssert = gen_assign_unroll_Str2Reg(var, fSimpUnroll);
    		if (toAssert != NULL) {
    			assert_axiom(toAssert);
    		}
    	}
    }

    if (opt_VerifyFinalCheckProgress && !finalCheckProgressIndicator) {
        TRACE("t_str", tout << "BUG: no progress in final check, giving up!!" << std::endl;);
        m.raise_exception("no progress in theory_str final check");
    }

    return FC_CONTINUE; // since by this point we've added axioms
}

inline std::string int_to_string(int i) {
	std::stringstream ss;
	ss << i;
	return ss.str();
}

inline std::string longlong_to_string(long long i) {
  std::stringstream ss;
  ss << i;
  return ss.str();
}

void theory_str::print_value_tester_list(svector<std::pair<int, expr*> > & testerList) {
	ast_manager & m = get_manager();
	TRACE("t_str_detail",
		int ss = testerList.size();
		tout << "valueTesterList = {";
		for (int i = 0; i < ss; ++i) {
			if (i % 4 == 0) {
				tout << std::endl;
			}
			tout << "(" << testerList[i].first << ", ";
			tout << mk_ismt2_pp(testerList[i].second, m);
			tout << "), ";
		}
		tout << std::endl << "}" << std::endl;
	);
}

std::string theory_str::gen_val_string(int len, int_vector & encoding) {
    SASSERT(charSetSize > 0);
    SASSERT(char_set != NULL);

    std::string re = std::string(len, char_set[0]);
    for (int i = 0; i < (int) encoding.size() - 1; i++) {
        int idx = encoding[i];
        re[len - 1 - i] = char_set[idx];
    }
    return re;
}

/*
 * The return value indicates whether we covered the search space.
 *   - If the next encoding is valid, return false
 *   - Otherwise, return true
 */
bool theory_str::get_next_val_encode(int_vector & base, int_vector & next) {
	SASSERT(charSetSize > 0);

    int s = 0;
    int carry = 0;
    next.reset();

    for (int i = 0; i < (int) base.size(); i++) {
        if (i == 0) {
            s = base[i] + 1;
            carry = s / charSetSize;
            s = s % charSetSize;
            next.push_back(s);
        } else {
            s = base[i] + carry;
            carry = s / charSetSize;
            s = s % charSetSize;
            next.push_back(s);
        }
    }
    if (next[next.size() - 1] > 0) {
        next.reset();
        return true;
    } else {
        return false;
    }
}

expr * theory_str::gen_val_options(expr * freeVar, expr * len_indicator, expr * val_indicator,
		std::string lenStr, int tries) {
	ast_manager & m = get_manager();
	context & ctx = get_context();

	int distance = 32;

	// ----------------------------------------------------------------------------------------
	// generate value options encoding
	// encoding is a vector of size (len + 1)
	// e.g, len = 2,
	//      encoding {1, 2, 0} means the value option is "charSet[2]"."charSet[1]"
	//      the last item in the encoding indicates whether the whole space is covered
	//      for example, if the charSet = {a, b}. All valid encodings are
	//        {0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}
	//      if add 1 to the last one, we get
	//        {0, 0, 1}
	//      the last item "1" shows this is not a valid encoding, and we have covered all space
	// ----------------------------------------------------------------------------------------
	int len = atoi(lenStr.c_str());
	bool coverAll = false;
	svector<int_vector> options;
	int_vector base;

	TRACE("t_str_detail", tout
			<< "freeVar = " << mk_ismt2_pp(freeVar, m) << std::endl
			<< "len_indicator = " << mk_ismt2_pp(len_indicator, m) << std::endl
			<< "val_indicator = " << mk_ismt2_pp(val_indicator, m) << std::endl
			<< "lenstr = " << lenStr << std::endl
			<< "tries = " << tries << std::endl;
            if (opt_AggressiveValueTesting) {
                tout << "note: aggressive value testing is enabled" << std::endl;
            }
	);

	if (tries == 0) {
		base = int_vector(len + 1, 0);
		coverAll = false;
	} else {
		expr * lastestValIndi = fvar_valueTester_map[freeVar][len][tries - 1].second;
		TRACE("t_str_detail", tout << "last value tester = " << mk_ismt2_pp(lastestValIndi, m) << std::endl;);
		coverAll = get_next_val_encode(val_range_map[lastestValIndi], base);
	}

	long long l = (tries) * distance;
	long long h = l;
	for (int i = 0; i < distance; i++) {
		if (coverAll)
			break;
		options.push_back(base);
		h++;
		coverAll = get_next_val_encode(options[options.size() - 1], base);
	}
	val_range_map[val_indicator] = options[options.size() - 1];

	TRACE("t_str_detail",
			tout << "value tester encoding " << "{" << std::endl;
		    int_vector vec = val_range_map[val_indicator];

		    for (int_vector::iterator it = vec.begin(); it != vec.end(); ++it) {
		    	tout << *it << std::endl;
		    }
			tout << "}" << std::endl;
	);

	// ----------------------------------------------------------------------------------------

	ptr_vector<expr> orList;
	ptr_vector<expr> andList;

	for (long long i = l; i < h; i++) {
		// TODO can we share the val_indicator constants with the length tester cache?
		orList.push_back(m.mk_eq(val_indicator, m_strutil.mk_string(longlong_to_string(i).c_str()) ));
		if (opt_AggressiveValueTesting) {
		    literal l = mk_eq(val_indicator, m_strutil.mk_string(longlong_to_string(i).c_str()), false);
		    ctx.mark_as_relevant(l);
		    ctx.force_phase(l);
		}

		std::string aStr = gen_val_string(len, options[i - l]);
		expr * strAst;
		if (opt_UseFastValueTesterCache) {
			if (!valueTesterCache.find(aStr, strAst)) {
				strAst = m_strutil.mk_string(aStr);
				valueTesterCache.insert(aStr, strAst);
				m_trail.push_back(strAst);
			}
		} else {
			strAst = m_strutil.mk_string(aStr);
		}
		andList.push_back(m.mk_eq(orList[orList.size() - 1], m.mk_eq(freeVar, strAst)));
	}
	if (!coverAll) {
		orList.push_back(m.mk_eq(val_indicator, m_strutil.mk_string("more")));
		if (opt_AggressiveValueTesting) {
		    literal l = mk_eq(val_indicator, m_strutil.mk_string("more"), false);
		    ctx.mark_as_relevant(l);
		    ctx.force_phase(~l);
		}
	}

	expr ** or_items = alloc_svect(expr*, orList.size());
	expr ** and_items = alloc_svect(expr*, andList.size() + 1);

	for (int i = 0; i < (int) orList.size(); i++) {
		or_items[i] = orList[i];
	}
	if (orList.size() > 1)
		and_items[0] = m.mk_or(orList.size(), or_items);
	else
		and_items[0] = or_items[0];

	for (int i = 0; i < (int) andList.size(); i++) {
		and_items[i + 1] = andList[i];
	}
	expr * valTestAssert = m.mk_and(andList.size() + 1, and_items);

	// ---------------------------------------
	// If the new value tester is $$_val_x_16_i
	// Should add ($$_len_x_j = 16) /\ ($$_val_x_16_i = "more")
	// ---------------------------------------
	andList.reset();
	andList.push_back(m.mk_eq(len_indicator, m_strutil.mk_string(lenStr.c_str())));
	for (int i = 0; i < tries; i++) {
		expr * vTester = fvar_valueTester_map[freeVar][len][i].second;
		if (vTester != val_indicator)
			andList.push_back(m.mk_eq(vTester, m_strutil.mk_string("more")));
	}
	expr * assertL = NULL;
	if (andList.size() == 1) {
		assertL = andList[0];
	} else {
		expr ** and_items = alloc_svect(expr*, andList.size());
		for (int i = 0; i < (int) andList.size(); i++) {
			and_items[i] = andList[i];
		}
		assertL = m.mk_and(andList.size(), and_items);
	}

	// (assertL => valTestAssert) <=> (!assertL OR valTestAssert)
	valTestAssert = m.mk_or(m.mk_not(assertL), valTestAssert);
	return valTestAssert;
}

expr * theory_str::gen_free_var_options(expr * freeVar, expr * len_indicator,
		std::string len_valueStr, expr * valTesterInCbEq, std::string valTesterValueStr) {
	ast_manager & m = get_manager();

	int len = atoi(len_valueStr.c_str());

	// check whether any value tester is actually in scope
	TRACE("t_str_detail", tout << "checking scope of previous value testers" << std::endl;);
	bool map_effectively_empty = true;
	if (fvar_valueTester_map[freeVar].find(len) != fvar_valueTester_map[freeVar].end()) {
	    // there's *something* in the map, but check its scope
	    svector<std::pair<int, expr*> > entries = fvar_valueTester_map[freeVar][len];
	    for (svector<std::pair<int,expr*> >::iterator it = entries.begin(); it != entries.end(); ++it) {
	        std::pair<int,expr*> entry = *it;
	        expr * aTester = entry.second;
	        if (internal_variable_set.find(aTester) == internal_variable_set.end()) {
	            TRACE("t_str_detail", tout << mk_pp(aTester, m) << " out of scope" << std::endl;);
	        } else {
	            TRACE("t_str_detail", tout << mk_pp(aTester, m) << " in scope" << std::endl;);
	            map_effectively_empty = false;
	            break;
	        }
	    }
	}

	if (map_effectively_empty) {
		TRACE("t_str_detail", tout << "no previous value testers, or none of them were in scope" << std::endl;);
		int tries = 0;
		expr * val_indicator = mk_internal_valTest_var(freeVar, len, tries);
		valueTester_fvar_map[val_indicator] = freeVar;
		fvar_valueTester_map[freeVar][len].push_back(std::make_pair(sLevel, val_indicator));
		print_value_tester_list(fvar_valueTester_map[freeVar][len]);
		return gen_val_options(freeVar, len_indicator, val_indicator, len_valueStr, tries);
	} else {
		TRACE("t_str_detail", tout << "checking previous value testers" << std::endl;);
		print_value_tester_list(fvar_valueTester_map[freeVar][len]);

		// go through all previous value testers
		// If some doesn't have an eqc value, add its assertion again.
		int testerTotal = fvar_valueTester_map[freeVar][len].size();
		int i = 0;
		for (; i < testerTotal; i++) {
			expr * aTester = fvar_valueTester_map[freeVar][len][i].second;

			// it's probably worth checking scope here, actually
			if (internal_variable_set.find(aTester) == internal_variable_set.end()) {
			    TRACE("t_str_detail", tout << "value tester " << mk_pp(aTester, m) << " out of scope, skipping" << std::endl;);
			    continue;
			}

			if (aTester == valTesterInCbEq) {
				break;
			}

			bool anEqcHasValue = false;
			// Z3_ast anEqc = get_eqc_value(t, aTester, anEqcHasValue);
			expr * aTester_eqc_value = get_eqc_value(aTester, anEqcHasValue);
			if (!anEqcHasValue) {
				TRACE("t_str_detail", tout << "value tester " << mk_ismt2_pp(aTester, m)
						<< " doesn't have an equivalence class value." << std::endl;);
				refresh_theory_var(aTester);

				expr * makeupAssert = gen_val_options(freeVar, len_indicator, aTester, len_valueStr, i);

				TRACE("t_str_detail", tout << "var: " << mk_ismt2_pp(freeVar, m) << std::endl
						<< mk_ismt2_pp(makeupAssert, m) << std::endl;);
				assert_axiom(makeupAssert);
			} else {
			    TRACE("t_str_detail", tout << "value tester " << mk_ismt2_pp(aTester, m)
			            << " == " << mk_ismt2_pp(aTester_eqc_value, m) << std::endl;);
			}
		}

		if (valTesterValueStr == "more") {
			expr * valTester = NULL;
			if (i + 1 < testerTotal) {
				valTester = fvar_valueTester_map[freeVar][len][i + 1].second;
				refresh_theory_var(valTester);
			} else {
				valTester = mk_internal_valTest_var(freeVar, len, i + 1);
				valueTester_fvar_map[valTester] = freeVar;
				fvar_valueTester_map[freeVar][len].push_back(std::make_pair(sLevel, valTester));
				print_value_tester_list(fvar_valueTester_map[freeVar][len]);
			}
			expr * nextAssert = gen_val_options(freeVar, len_indicator, valTester, len_valueStr, i + 1);
			return nextAssert;
		}

		return NULL;
	}
}

void theory_str::reduce_virtual_regex_in(expr * var, expr * regex, expr_ref_vector & items) {
	context & ctx = get_context();
	ast_manager & mgr = get_manager();

	TRACE("t_str_detail", tout << "reduce regex " << mk_pp(regex, mgr) << " with respect to variable " << mk_pp(var, mgr) << std::endl;);

	app * regexFuncDecl = to_app(regex);
	if (is_Str2Reg(regexFuncDecl)) {
		// ---------------------------------------------------------
		// var \in Str2Reg(s1)
		//   ==>
		// var = s1 /\ length(var) = length(s1)
		// ---------------------------------------------------------
		expr * strInside = to_app(regex)->get_arg(0);
		items.push_back(ctx.mk_eq_atom(var, strInside));
		items.push_back(ctx.mk_eq_atom(mk_strlen(var), mk_strlen(strInside)));
		return;
	}
	// RegexUnion
	else if (is_RegexUnion(regexFuncDecl)) {
		// ---------------------------------------------------------
		// var \in RegexUnion(r1, r2)
		//   ==>
		// (var = newVar1 \/ var = newVar2)
		// (var = newVar1 --> length(var) = length(newVar1)) /\ (var = newVar2 --> length(var) = length(newVar2))
		//  /\ (newVar1 \in r1) /\  (newVar2 \in r2)
		// ---------------------------------------------------------
		expr_ref newVar1(mk_regex_rep_var(), mgr);
		expr_ref newVar2(mk_regex_rep_var(), mgr);
		items.push_back(mgr.mk_or(ctx.mk_eq_atom(var, newVar1), ctx.mk_eq_atom(var, newVar2)));
		items.push_back(mgr.mk_or(
				mgr.mk_not(ctx.mk_eq_atom(var, newVar1)),
				ctx.mk_eq_atom(mk_strlen(var), mk_strlen(newVar1))));
		items.push_back(mgr.mk_or(
				mgr.mk_not(ctx.mk_eq_atom(var, newVar2)),
				ctx.mk_eq_atom(mk_strlen(var), mk_strlen(newVar2))));

		expr * regArg1 = to_app(regex)->get_arg(0);
		reduce_virtual_regex_in(newVar1, regArg1, items);

		expr * regArg2 = to_app(regex)->get_arg(1);
		reduce_virtual_regex_in(newVar2, regArg2, items);

		return;
	}
	// RegexConcat
	else if (is_RegexConcat(regexFuncDecl)) {
		// ---------------------------------------------------------
		// var \in RegexConcat(r1, r2)
		//   ==>
		//    (var = newVar1 . newVar2) /\ (length(var) = length(vewVar1 . newVar2) )
		// /\ (newVar1 \in r1) /\  (newVar2 \in r2)
		// ---------------------------------------------------------
		expr_ref newVar1(mk_regex_rep_var(), mgr);
		expr_ref newVar2(mk_regex_rep_var(), mgr);
		expr_ref concatAst(mk_concat(newVar1, newVar2), mgr);
		items.push_back(ctx.mk_eq_atom(var, concatAst));
		items.push_back(ctx.mk_eq_atom(mk_strlen(var),
				m_autil.mk_add(mk_strlen(newVar1), mk_strlen(newVar2))));

		expr * regArg1 = to_app(regex)->get_arg(0);
		reduce_virtual_regex_in(newVar1, regArg1, items);
		expr * regArg2 = to_app(regex)->get_arg(1);
		reduce_virtual_regex_in(newVar2, regArg2, items);
		return;
	}
	// Unroll
	else if (is_RegexStar(regexFuncDecl)) {
		// ---------------------------------------------------------
		// var \in Star(r1)
		//   ==>
		// var = unroll(r1, t1) /\ |var| = |unroll(r1, t1)|
		// ---------------------------------------------------------
		expr * regArg = to_app(regex)->get_arg(0);
		expr_ref unrollCnt(mk_unroll_bound_var(), mgr);
		expr_ref unrollFunc(mk_unroll(regArg, unrollCnt), mgr);
		items.push_back(ctx.mk_eq_atom(var, unrollFunc));
		items.push_back(ctx.mk_eq_atom(mk_strlen(var), mk_strlen(unrollFunc)));
		return;
	} else {
		UNREACHABLE();
	}
}

void theory_str::gen_assign_unroll_reg(std::set<expr*> & unrolls) {
	context & ctx = get_context();
	ast_manager & mgr = get_manager();

	expr_ref_vector items(mgr);
	for (std::set<expr*>::iterator itor = unrolls.begin(); itor != unrolls.end(); itor++) {
		expr * unrFunc = *itor;
		TRACE("t_str_detail", tout << "generating assignment for unroll " << mk_pp(unrFunc, mgr) << std::endl;);

		expr * regexInUnr = to_app(unrFunc)->get_arg(0);
		expr * cntInUnr = to_app(unrFunc)->get_arg(1);
		items.reset();

		rational low, high;
		bool low_exists = lower_bound(cntInUnr, low);
		bool high_exists = upper_bound(cntInUnr, high);

		TRACE("t_str_detail",
				tout << "unroll " << mk_pp(unrFunc, mgr) << std::endl;
				rational unrLenValue;
				bool unrLenValue_exists = get_len_value(unrFunc, unrLenValue);
				tout << "unroll length: " << (unrLenValue_exists ? unrLenValue.to_string() : "?") << std::endl;
				rational cntInUnrValue;
				bool cntHasValue = get_value(cntInUnr, cntInUnrValue);
				tout << "unroll count: " << (cntHasValue ? cntInUnrValue.to_string() : "?")
						<< " low = "
						<< (low_exists ? low.to_string() : "?")
						<< " high = "
						<< (high_exists ? high.to_string() : "?")
						<< std::endl;
			);

		expr_ref toAssert(mgr);
		if (low.is_neg()) {
			toAssert = m_autil.mk_ge(cntInUnr, mk_int(0));
		} else {
			if (unroll_var_map.find(unrFunc) == unroll_var_map.end()) {

				expr_ref newVar1(mk_regex_rep_var(), mgr);
				expr_ref newVar2(mk_regex_rep_var(), mgr);
				expr_ref concatAst(mk_concat(newVar1, newVar2), mgr);
				expr_ref newCnt(mk_unroll_bound_var(), mgr);
				expr_ref newUnrollFunc(mk_unroll(regexInUnr, newCnt), mgr);

				// unroll(r1, t1) = newVar1 . newVar2
				items.push_back(ctx.mk_eq_atom(unrFunc, concatAst));
				items.push_back(ctx.mk_eq_atom(mk_strlen(unrFunc), m_autil.mk_add(mk_strlen(newVar1), mk_strlen(newVar2))));
				// mk_strlen(unrFunc) >= mk_strlen(newVar{1,2})
				items.push_back(m_autil.mk_ge(m_autil.mk_add(mk_strlen(unrFunc), m_autil.mk_mul(mk_int(-1), mk_strlen(newVar1))), mk_int(0)));
				items.push_back(m_autil.mk_ge(m_autil.mk_add(mk_strlen(unrFunc), m_autil.mk_mul(mk_int(-1), mk_strlen(newVar2))), mk_int(0)));
				// newVar1 \in r1
				reduce_virtual_regex_in(newVar1, regexInUnr, items);
				items.push_back(ctx.mk_eq_atom(cntInUnr, m_autil.mk_add(newCnt, mk_int(1))));
				items.push_back(ctx.mk_eq_atom(newVar2, newUnrollFunc));
				items.push_back(ctx.mk_eq_atom(mk_strlen(newVar2), mk_strlen(newUnrollFunc)));
				toAssert = ctx.mk_eq_atom(
						m_autil.mk_ge(cntInUnr, mk_int(1)),
						mk_and(items));

				// option 0
				expr_ref op0(ctx.mk_eq_atom(cntInUnr, mk_int(0)), mgr);
				expr_ref ast1(ctx.mk_eq_atom(unrFunc, m_strutil.mk_string("")), mgr);
				expr_ref ast2(ctx.mk_eq_atom(mk_strlen(unrFunc), mk_int(0)), mgr);
				expr_ref and1(mgr.mk_and(ast1, ast2), mgr);

				// put together
				toAssert = mgr.mk_and(ctx.mk_eq_atom(op0, and1), toAssert);

				unroll_var_map[unrFunc] = toAssert;
			} else {
				toAssert = unroll_var_map[unrFunc];
			}
		}
		m_trail.push_back(toAssert);
		assert_axiom(toAssert);
	}
}

static int computeGCD(int x, int y) {
	if (x == 0) {
		return y;
	}
	while (y != 0) {
		if (x > y) {
			x = x - y;
		} else {
			y = y - x;
		}
	}
	return x;
}

static int computeLCM(int a, int b) {
	int temp = computeGCD(a, b);
	return temp ? (a / temp * b) : 0;
}

static std::string get_unrolled_string(std::string core, int count) {
	std::string res = "";
	for (int i = 0; i < count; i++) {
		res += core;
	}
	return res;
}

expr * theory_str::gen_assign_unroll_Str2Reg(expr * n, std::set<expr*> & unrolls) {
	context & ctx = get_context();
	ast_manager & mgr = get_manager();

	int lcm = 1;
	int coreValueCount = 0;
	expr * oneUnroll = NULL;
	std::string oneCoreStr = "";
	for (std::set<expr*>::iterator itor = unrolls.begin(); itor != unrolls.end(); itor++) {
		expr * str2RegFunc = to_app(*itor)->get_arg(0);
		expr * coreVal = to_app(str2RegFunc)->get_arg(0);
		std::string coreStr = m_strutil.get_string_constant_value(coreVal);
		if (oneUnroll == NULL) {
			oneUnroll = *itor;
			oneCoreStr = coreStr;
		}
		coreValueCount++;
		int core1Len = coreStr.length();
		lcm = computeLCM(lcm, core1Len);
	}
	//
	bool canHaveNonEmptyAssign = true;
	expr_ref_vector litems(mgr);
	std::string lcmStr = get_unrolled_string(oneCoreStr, (lcm / oneCoreStr.length()));
	for (std::set<expr*>::iterator itor = unrolls.begin(); itor != unrolls.end(); itor++) {
		expr * str2RegFunc = to_app(*itor)->get_arg(0);
		expr * coreVal = to_app(str2RegFunc)->get_arg(0);
		std::string coreStr = m_strutil.get_string_constant_value(coreVal);
		int core1Len = coreStr.length();
		std::string uStr = get_unrolled_string(coreStr, (lcm / core1Len));
		if (uStr != lcmStr) {
			canHaveNonEmptyAssign = false;
		}
		litems.push_back(ctx.mk_eq_atom(n, *itor));
	}

	if (canHaveNonEmptyAssign) {
		return gen_unroll_conditional_options(n, unrolls, lcmStr);
	} else {
		expr_ref implyL(mk_and(litems), mgr);
		expr_ref implyR(ctx.mk_eq_atom(n, m_strutil.mk_string("")), mgr);
		// want to return (implyL -> implyR)
		expr * final_axiom = rewrite_implication(implyL, implyR);
		return final_axiom;
	}
}

expr * theory_str::gen_unroll_conditional_options(expr * var, std::set<expr*> & unrolls, std::string lcmStr) {
	context & ctx = get_context();
	ast_manager & mgr = get_manager();

	int dist = opt_LCMUnrollStep;
	expr_ref_vector litems(mgr);
	expr_ref moreAst(m_strutil.mk_string("more"), mgr);
	for (std::set<expr*>::iterator itor = unrolls.begin(); itor != unrolls.end(); itor++) {
		expr_ref item(ctx.mk_eq_atom(var, *itor), mgr);
		TRACE("t_str_detail", tout << "considering unroll " << mk_pp(item, mgr) << std::endl;);
		litems.push_back(item);
	}

	// handle out-of-scope entries in unroll_tries_map

	ptr_vector<expr> outOfScopeTesters;
	// TODO refactor unroll_tries_map and internal_unrollTest_vars to use m_trail_stack

	for (ptr_vector<expr>::iterator it = unroll_tries_map[var][unrolls].begin();
	        it != unroll_tries_map[var][unrolls].end(); ++it) {
	    expr * tester = *it;
	    bool inScope = (internal_unrollTest_vars.find(tester) != internal_unrollTest_vars.end());
	    TRACE("t_str_detail", tout << "unroll test var " << mk_pp(tester, mgr)
	            << (inScope ? " in scope" : " out of scope")
	            << std::endl;);
	    if (!inScope) {
	        outOfScopeTesters.push_back(tester);
	    }
	}

	for (ptr_vector<expr>::iterator it = outOfScopeTesters.begin();
	        it != outOfScopeTesters.end(); ++it) {
	    unroll_tries_map[var][unrolls].erase(*it);
	}


	if (unroll_tries_map[var][unrolls].size() == 0) {
		unroll_tries_map[var][unrolls].push_back(mk_unroll_test_var());
	}

	int tries = unroll_tries_map[var][unrolls].size();
	for (int i = 0; i < tries; i++) {
		expr * tester = unroll_tries_map[var][unrolls][i];
		// TESTING
		refresh_theory_var(tester);
		bool testerHasValue = false;
		expr * testerVal = get_eqc_value(tester, testerHasValue);
		if (!testerHasValue) {
			// generate make-up assertion
			int l = i * dist;
			int h = (i + 1) * dist;
			expr_ref lImp(mk_and(litems), mgr);
			expr_ref rImp(gen_unroll_assign(var, lcmStr, tester, l, h), mgr);

			SASSERT(lImp);
			TRACE("t_str_detail", tout << "lImp = " << mk_pp(lImp, mgr) << std::endl;);
			SASSERT(rImp);
			TRACE("t_str_detail", tout << "rImp = " << mk_pp(rImp, mgr) << std::endl;);

			expr_ref toAssert(mgr.mk_or(mgr.mk_not(lImp), rImp), mgr);
			SASSERT(toAssert);
			TRACE("t_str_detail", tout << "Making up assignments for variable which is equal to unbounded Unroll" << std::endl;);
			m_trail.push_back(toAssert);
			return toAssert;

			// note: this is how the code looks in Z3str2's strRegex.cpp:genUnrollConditionalOptions.
			// the return is in the same place

			// insert [tester = "more"] to litems so that the implyL for next tester is correct
			litems.push_back(ctx.mk_eq_atom(tester, moreAst));
		} else {
			std::string testerStr = m_strutil.get_string_constant_value(testerVal);
			TRACE("t_str_detail", tout << "Tester [" << mk_pp(tester, mgr) << "] = " << testerStr << std::endl;);
			if (testerStr == "more") {
				litems.push_back(ctx.mk_eq_atom(tester, moreAst));
			}
		}
	}
	expr * tester = mk_unroll_test_var();
	unroll_tries_map[var][unrolls].push_back(tester);
	int l = tries * dist;
	int h = (tries + 1) * dist;
	expr_ref lImp(mk_and(litems), mgr);
	expr_ref rImp(gen_unroll_assign(var, lcmStr, tester, l, h), mgr);
	SASSERT(lImp);
	SASSERT(rImp);
	expr_ref toAssert(mgr.mk_or(mgr.mk_not(lImp), rImp), mgr);
	SASSERT(toAssert);
	TRACE("t_str_detail", tout << "Generating assignment for variable which is equal to unbounded Unroll" << std::endl;);
	m_trail.push_back(toAssert);
	return toAssert;
}

expr * theory_str::gen_unroll_assign(expr * var, std::string lcmStr, expr * testerVar, int l, int h) {
	context & ctx = get_context();
	ast_manager & mgr = get_manager();

	TRACE("t_str_detail", tout << "entry: var = " << mk_pp(var, mgr) << ", lcmStr = " << lcmStr
			<< ", l = " << l << ", h = " << h << std::endl;);

	if (opt_AggressiveUnrollTesting) {
	    TRACE("t_str_detail", tout << "note: aggressive unroll testing is active" << std::endl;);
	}

	expr_ref_vector orItems(mgr);
	expr_ref_vector andItems(mgr);

	for (int i = l; i < h; i++) {
		std::string iStr = int_to_string(i);
		expr_ref testerEqAst(ctx.mk_eq_atom(testerVar, m_strutil.mk_string(iStr)), mgr);
		TRACE("t_str_detail", tout << "testerEqAst = " << mk_pp(testerEqAst, mgr) << std::endl;);
		if (opt_AggressiveUnrollTesting) {
		    literal l = mk_eq(testerVar, m_strutil.mk_string(iStr), false);
		    ctx.mark_as_relevant(l);
		    ctx.force_phase(l);
		}

		orItems.push_back(testerEqAst);
		std::string unrollStrInstance = get_unrolled_string(lcmStr, i);

		expr_ref x1(ctx.mk_eq_atom(testerEqAst, ctx.mk_eq_atom(var, m_strutil.mk_string(unrollStrInstance))), mgr);
		TRACE("t_str_detail", tout << "x1 = " << mk_pp(x1, mgr) << std::endl;);
		andItems.push_back(x1);

		expr_ref x2(ctx.mk_eq_atom(testerEqAst, ctx.mk_eq_atom(mk_strlen(var), mk_int(i * lcmStr.length()))), mgr);
		TRACE("t_str_detail", tout << "x2 = " << mk_pp(x2, mgr) << std::endl;);
		andItems.push_back(x2);
	}
	expr_ref testerEqMore(ctx.mk_eq_atom(testerVar, m_strutil.mk_string("more")), mgr);
	TRACE("t_str_detail", tout << "testerEqMore = " << mk_pp(testerEqMore, mgr) << std::endl;);
	if (opt_AggressiveUnrollTesting) {
	    literal l = mk_eq(testerVar, m_strutil.mk_string("more"), false);
	    ctx.mark_as_relevant(l);
	    ctx.force_phase(~l);
	}

	orItems.push_back(testerEqMore);
	int nextLowerLenBound = h * lcmStr.length();
	expr_ref more2(ctx.mk_eq_atom(testerEqMore,
			//Z3_mk_ge(mk_length(t, var), mk_int(ctx, nextLowerLenBound))
			m_autil.mk_ge(m_autil.mk_add(mk_strlen(var), mk_int(-1 * nextLowerLenBound)), mk_int(0))
			), mgr);
	TRACE("t_str_detail", tout << "more2 = " << mk_pp(more2, mgr) << std::endl;);
	andItems.push_back(more2);

	expr_ref finalOR(mgr.mk_or(orItems.size(), orItems.c_ptr()), mgr);
	TRACE("t_str_detail", tout << "finalOR = " << mk_pp(finalOR, mgr) << std::endl;);
	andItems.push_back(mk_or(orItems));

	expr_ref finalAND(mgr.mk_and(andItems.size(), andItems.c_ptr()), mgr);
	TRACE("t_str_detail", tout << "finalAND = " << mk_pp(finalAND, mgr) << std::endl;);

	// doing the following avoids a segmentation fault
	m_trail.push_back(finalAND);
	return finalAND;
}

expr * theory_str::gen_len_test_options(expr * freeVar, expr * indicator, int tries) {
	ast_manager & m = get_manager();
	context & ctx = get_context();

	expr_ref freeVarLen(mk_strlen(freeVar), m);
	SASSERT(freeVarLen);

	expr_ref_vector orList(m);
	expr_ref_vector andList(m);

	int distance = 3;
	int l = (tries - 1) * distance;
	int h = tries * distance;

	TRACE("t_str_detail",
	        tout << "building andList and orList" << std::endl;
	        if (opt_AggressiveLengthTesting) {
	            tout << "note: aggressive length testing is active" << std::endl;
	        }
	);

	for (int i = l; i < h; ++i) {
	    expr_ref str_indicator(m);
	    if (opt_UseFastLengthTesterCache) {
	        rational ri(i);
	        expr * lookup_val;
	        if(lengthTesterCache.find(ri, lookup_val)) {
	            str_indicator = expr_ref(lookup_val, m);
	        } else {
	            // no match; create and insert
	            std::string i_str = int_to_string(i);
                expr_ref new_val(m_strutil.mk_string(i_str), m);
                lengthTesterCache.insert(ri, new_val);
                m_trail.push_back(new_val);
                str_indicator = expr_ref(new_val, m);
	        }
	    } else {
	        std::string i_str = int_to_string(i);
	        str_indicator = expr_ref(m_strutil.mk_string(i_str), m);
	    }
		expr_ref or_expr(ctx.mk_eq_atom(indicator, str_indicator), m);
		orList.push_back(or_expr);

		if (opt_AggressiveLengthTesting) {
		    literal l = mk_eq(indicator, str_indicator, false);
		    ctx.mark_as_relevant(l);
		    ctx.force_phase(l);
		}

		expr_ref and_expr(ctx.mk_eq_atom(orList.get(orList.size() - 1), m.mk_eq(freeVarLen, mk_int(i))), m);
		andList.push_back(and_expr);
	}

	// TODO cache mk_string("more")
	orList.push_back(m.mk_eq(indicator, m_strutil.mk_string("more")));
	if (opt_AggressiveLengthTesting) {
	    literal l = mk_eq(indicator, m_strutil.mk_string("more"), false);
	    ctx.mark_as_relevant(l);
	    ctx.force_phase(~l);
	}

	andList.push_back(ctx.mk_eq_atom(orList.get(orList.size() - 1), m_autil.mk_ge(freeVarLen, mk_int(h))));

	expr_ref_vector or_items(m);
	expr_ref_vector and_items(m);

	for (unsigned i = 0; i < orList.size(); ++i) {
		or_items.push_back(orList.get(i));
	}

	and_items.push_back(mk_or(or_items));
	for(unsigned i = 0; i < andList.size(); ++i) {
		and_items.push_back(andList.get(i));
	}

	TRACE("t_str_detail", tout << "check: " << mk_pp(mk_and(and_items), m) << std::endl;);

	expr_ref lenTestAssert = mk_and(and_items);
	SASSERT(lenTestAssert);
	TRACE("t_str_detail", tout << "crash avoidance lenTestAssert: " << mk_pp(lenTestAssert, m) << std::endl;);

	int testerCount = tries - 1;
	if (testerCount > 0) {
	    expr_ref_vector and_items_LHS(m);
		expr_ref moreAst(m_strutil.mk_string("more"), m);
		for (int i = 0; i < testerCount; ++i) {
		    expr * indicator = fvar_lenTester_map[freeVar][i];
		    if (internal_variable_set.find(indicator) == internal_variable_set.end()) {
		        TRACE("t_str_detail", tout << "indicator " << mk_pp(indicator, m) << " out of scope; continuing" << std::endl;);
		        continue;
		    } else {
		        TRACE("t_str_detail", tout << "indicator " << mk_pp(indicator, m) << " in scope" << std::endl;);
		        and_items_LHS.push_back(ctx.mk_eq_atom(indicator, moreAst));
		    }
		}
		expr_ref assertL(mk_and(and_items_LHS), m);
		SASSERT(assertL);
		expr * finalAxiom = m.mk_or(m.mk_not(assertL), lenTestAssert.get());
		SASSERT(finalAxiom != NULL);
		TRACE("t_str_detail", tout << "crash avoidance finalAxiom: " << mk_pp(finalAxiom, m) << std::endl;);
		return finalAxiom;
	} else {
	    TRACE("t_str_detail", tout << "crash avoidance lenTestAssert.get(): " << mk_pp(lenTestAssert.get(), m) << std::endl;);
	    m_trail.push_back(lenTestAssert.get());
	    return lenTestAssert.get();
	}
}

// -----------------------------------------------------------------------------------------------------
// True branch will be taken in final_check:
//   - When we discover a variable is "free" for the first time
//     lenTesterInCbEq = NULL
//     lenTesterValue = ""
// False branch will be taken when invoked by new_eq_eh().
//   - After we set up length tester for a "free" var in final_check,
//     when the tester is assigned to some value (e.g. "more" or "4"),
//     lenTesterInCbEq != NULL, and its value will be passed by lenTesterValue
// The difference is that in new_eq_eh(), lenTesterInCbEq and its value have NOT been put into a same eqc
// -----------------------------------------------------------------------------------------------------
expr * theory_str::gen_len_val_options_for_free_var(expr * freeVar, expr * lenTesterInCbEq, std::string lenTesterValue) {

	ast_manager & m = get_manager();

	TRACE("t_str_detail", tout << "gen for free var " << mk_ismt2_pp(freeVar, m) << std::endl;);

	bool map_effectively_empty = false;
	if (fvar_len_count_map.find(freeVar) == fvar_len_count_map.end()) {
	    TRACE("t_str_detail", tout << "fvar_len_count_map is empty" << std::endl;);
	    map_effectively_empty = true;
	}

	if (!map_effectively_empty) {
	    // check whether any entries correspond to variables that went out of scope;
	    // if every entry is out of scope then the map counts as being empty
	    // TODO: maybe remove them from the map instead? either here or in pop_scope_eh()

	    // assume empty and find a counterexample
	    map_effectively_empty = true;
        ptr_vector<expr> indicator_set = fvar_lenTester_map[freeVar];
        for (ptr_vector<expr>::iterator it = indicator_set.begin(); it != indicator_set.end(); ++it) {
            expr * indicator = *it;
            if (internal_variable_set.find(indicator) != internal_variable_set.end()) {
                TRACE("t_str_detail", tout <<"found active internal variable " << mk_ismt2_pp(indicator, m)
                        << " in fvar_lenTester_map[freeVar]" << std::endl;);
                map_effectively_empty = false;
                break;
            }
        }
        CTRACE("t_str_detail", map_effectively_empty, tout << "all variables in fvar_lenTester_map[freeVar] out of scope" << std::endl;);
	}

	if (map_effectively_empty) {
	    // no length assertions for this free variable have ever been added.
		TRACE("t_str_detail", tout << "no length assertions yet" << std::endl;);

		fvar_len_count_map[freeVar] = 1;
		unsigned int testNum = fvar_len_count_map[freeVar];

		expr_ref indicator(mk_internal_lenTest_var(freeVar, testNum), m);
		SASSERT(indicator);

		// since the map is "effectively empty", we can remove those variables that have left scope...
		fvar_lenTester_map[freeVar].shrink(0);
		fvar_lenTester_map[freeVar].push_back(indicator);
		lenTester_fvar_map[indicator] = freeVar;

		expr * lenTestAssert = gen_len_test_options(freeVar, indicator, testNum);
		SASSERT(lenTestAssert != NULL);
		return lenTestAssert;
	} else {
		TRACE("t_str_detail", tout << "found previous in-scope length assertions" << std::endl;);

		expr * effectiveLenInd = NULL;
		std::string effectiveLenIndiStr = "";
		int lenTesterCount = (int) fvar_lenTester_map[freeVar].size();

		TRACE("t_str_detail",
		        tout << lenTesterCount << " length testers in fvar_lenTester_map[" << mk_pp(freeVar, m) << "]:" << std::endl;
		        for (int i = 0; i < lenTesterCount; ++i) {
		            expr * len_indicator = fvar_lenTester_map[freeVar][i];
		            tout << mk_pp(len_indicator, m) << ": ";
		            bool effectiveInScope = (internal_variable_set.find(len_indicator) != internal_variable_set.end());
		            tout << (effectiveInScope ? "in scope" : "NOT in scope");
		            tout << std::endl;
		        }
		        );

		int i = 0;
		for (; i < lenTesterCount; ++i) {
			expr * len_indicator_pre = fvar_lenTester_map[freeVar][i];
			// check whether this is in scope as well
			if (internal_variable_set.find(len_indicator_pre) == internal_variable_set.end()) {
			    TRACE("t_str_detail", tout << "length indicator " << mk_pp(len_indicator_pre, m) << " not in scope" << std::endl;);
			    continue;
			}

			bool indicatorHasEqcValue = false;
			expr * len_indicator_value = get_eqc_value(len_indicator_pre, indicatorHasEqcValue);
			TRACE("t_str_detail", tout << "length indicator " << mk_ismt2_pp(len_indicator_pre, m) <<
					" = " << mk_ismt2_pp(len_indicator_value, m) << std::endl;);
			if (indicatorHasEqcValue) {
				const char * val = 0;
				m_strutil.is_string(len_indicator_value, & val);
				std::string len_pIndiStr(val);
				if (len_pIndiStr != "more") {
					effectiveLenInd = len_indicator_pre;
					effectiveLenIndiStr = len_pIndiStr;
					break;
				}
			} else {
				if (lenTesterInCbEq != len_indicator_pre) {
					TRACE("t_str", tout << "WARNING: length indicator " << mk_ismt2_pp(len_indicator_pre, m)
							<< " does not have an equivalence class value."
							<< " i = " << i << ", lenTesterCount = " << lenTesterCount << std::endl;);
					if (i > 0) {
						effectiveLenInd = fvar_lenTester_map[freeVar][i - 1];
						bool effectiveHasEqcValue;
						expr * effective_eqc_value = get_eqc_value(effectiveLenInd, effectiveHasEqcValue);
						bool effectiveInScope = (internal_variable_set.find(effectiveLenInd) != internal_variable_set.end());
						TRACE("t_str_detail", tout << "checking effective length indicator " << mk_pp(effectiveLenInd, m) << ": "
						        << (effectiveInScope ? "in scope" : "NOT in scope") << ", ";
						        if (effectiveHasEqcValue) {
						            tout << "~= " << mk_pp(effective_eqc_value, m);
						        } else {
						            tout << "no eqc string constant";
						        }
						        tout << std::endl;);
						if (effectiveLenInd == lenTesterInCbEq) {
							effectiveLenIndiStr = lenTesterValue;
						} else {
						    if (effectiveHasEqcValue) {
						        effectiveLenIndiStr = m_strutil.get_string_constant_value(effective_eqc_value);
						    } else {
						        // TODO this should be unreachable, but can we really do anything here?
						        NOT_IMPLEMENTED_YET();
						    }
						}
					}
					break;
				}
				// lenTesterInCbEq == len_indicator_pre
				else {
					if (lenTesterValue != "more") {
						effectiveLenInd = len_indicator_pre;
						effectiveLenIndiStr = lenTesterValue;
						break;
					}
				}
			} // !indicatorHasEqcValue
		} // for (i : [0..lenTesterCount-1])
		if (effectiveLenIndiStr == "more" || effectiveLenIndiStr == "") {
			TRACE("t_str", tout << "length is not fixed; generating length tester options for free var" << std::endl;);
			expr_ref indicator(m);
			unsigned int testNum = 0;

			TRACE("t_str", tout << "effectiveLenIndiStr = " << effectiveLenIndiStr
					<< ", i = " << i << ", lenTesterCount = " << lenTesterCount << std::endl;);

			if (i == lenTesterCount) {
				fvar_len_count_map[freeVar] = fvar_len_count_map[freeVar] + 1;
				testNum = fvar_len_count_map[freeVar];
				indicator = mk_internal_lenTest_var(freeVar, testNum);
				fvar_lenTester_map[freeVar].push_back(indicator);
				lenTester_fvar_map[indicator] = freeVar;
			} else {
			    // TODO make absolutely sure this is safe to do if 'indicator' is technically out of scope
				indicator = fvar_lenTester_map[freeVar][i];
				refresh_theory_var(indicator);
				testNum = i + 1;
			}
			expr * lenTestAssert = gen_len_test_options(freeVar, indicator, testNum);
			SASSERT(lenTestAssert != NULL);
			return lenTestAssert;
		} else {
			TRACE("t_str", tout << "length is fixed; generating models for free var" << std::endl;);
			// length is fixed
			expr * valueAssert = gen_free_var_options(freeVar, effectiveLenInd, effectiveLenIndiStr, NULL, "");
			return valueAssert;
		}
	} // fVarLenCountMap.find(...)
}

void theory_str::get_concats_in_eqc(expr * n, std::set<expr*> & concats) {
    context & ctx = get_context();

    expr * eqcNode = n;
    do {
        if (is_concat(to_app(eqcNode))) {
            concats.insert(eqcNode);
        }
        eqcNode = get_eqc_next(eqcNode);
    } while (eqcNode != n);
}

void theory_str::get_var_in_eqc(expr * n, std::set<expr*> & varSet) {
	context & ctx = get_context();

	expr * eqcNode = n;
	do {
		if (variable_set.find(eqcNode) != variable_set.end()) {
			varSet.insert(eqcNode);
		}
		eqcNode = get_eqc_next(eqcNode);
	} while (eqcNode != n);
}

bool cmpvarnames(expr * lhs, expr * rhs) {
    symbol lhs_name = to_app(lhs)->get_decl()->get_name();
    symbol rhs_name = to_app(rhs)->get_decl()->get_name();
    return lhs_name.str() < rhs_name.str();
}

void theory_str::process_free_var(std::map<expr*, int> & freeVar_map) {
	context & ctx = get_context();
	ast_manager & m = get_manager();

	std::set<expr*> eqcRepSet;
	std::set<expr*> leafVarSet;
	std::map<int, std::set<expr*> > aloneVars;

	for (std::map<expr*, int>::iterator fvIt = freeVar_map.begin(); fvIt != freeVar_map.end(); fvIt++) {
		expr * freeVar = fvIt->first;
		// skip all regular expression vars
		if (regex_variable_set.find(freeVar) != regex_variable_set.end()) {
			continue;
		}

		// Iterate the EQC of freeVar, its eqc variable should not be in the eqcRepSet.
		// If found, have to filter it out
		std::set<expr*> eqVarSet;
		get_var_in_eqc(freeVar, eqVarSet);
		bool duplicated = false;
		expr * dupVar = NULL;
		for (std::set<expr*>::iterator itorEqv = eqVarSet.begin(); itorEqv != eqVarSet.end(); itorEqv++) {
			if (eqcRepSet.find(*itorEqv) != eqcRepSet.end()) {
				duplicated = true;
				dupVar = *itorEqv;
				break;
			}
		}
		if (duplicated && dupVar != NULL) {
			TRACE("t_str_detail", tout << "Duplicated free variable found:" << mk_ismt2_pp(freeVar, m)
					<< " = " << mk_ismt2_pp(dupVar, m) << " (SKIP)" << std::endl;);
			continue;
		} else {
			eqcRepSet.insert(freeVar);
		}
	}

	for (std::set<expr*>::iterator fvIt = eqcRepSet.begin(); fvIt != eqcRepSet.end(); fvIt++) {
		bool standAlone = true;
		expr * freeVar = *fvIt;
		// has length constraint initially
		if (input_var_in_len.find(freeVar) != input_var_in_len.end()) {
			standAlone = false;
		}
		// iterate parents
		if (standAlone) {
			// I hope this works!
			enode * e_freeVar = ctx.get_enode(freeVar);
			enode_vector::iterator it = e_freeVar->begin_parents();
			for (; it != e_freeVar->end_parents(); ++it) {
				expr * parentAst = (*it)->get_owner();
				if (is_concat(to_app(parentAst))) {
					standAlone = false;
					break;
				}
			}
		}

		if (standAlone) {
		    rational len_value;
		    bool len_value_exists = get_len_value(freeVar, len_value);
			if (len_value_exists) {
				leafVarSet.insert(freeVar);
			} else {
				aloneVars[-1].insert(freeVar);
			}
		} else {
			leafVarSet.insert(freeVar);
		}
	}

	// TODO here's a great place for debugging info

	// testing: iterate over leafVarSet deterministically
	if (false) {
	    // *** TESTING CODE
	    std::vector<expr*> sortedLeafVarSet;
	    for (std::set<expr*>::iterator itor1 = leafVarSet.begin(); itor1 != leafVarSet.end(); ++itor1) {
	        sortedLeafVarSet.push_back(*itor1);
	    }
	    std::sort(sortedLeafVarSet.begin(), sortedLeafVarSet.end(), cmpvarnames);
	    for(std::vector<expr*>::iterator itor1 = sortedLeafVarSet.begin();
	            itor1 != sortedLeafVarSet.end(); ++itor1) {
	        expr * toAssert = gen_len_val_options_for_free_var(*itor1, NULL, "");
	        // gen_len_val_options_for_free_var() can legally return NULL,
	        // as methods that it calls may assert their own axioms instead.
	        if (toAssert != NULL) {
	            assert_axiom(toAssert);
	        }
	    }
	} else {
	    // *** CODE FROM BEFORE
	    for(std::set<expr*>::iterator itor1 = leafVarSet.begin();
	            itor1 != leafVarSet.end(); ++itor1) {
	        expr * toAssert = gen_len_val_options_for_free_var(*itor1, NULL, "");
	        // gen_len_val_options_for_free_var() can legally return NULL,
	        // as methods that it calls may assert their own axioms instead.
	        if (toAssert != NULL) {
	            assert_axiom(toAssert);
	        }
	    }
	}

	for (std::map<int, std::set<expr*> >::iterator mItor = aloneVars.begin();
			mItor != aloneVars.end(); ++mItor) {
		std::set<expr*>::iterator itor2 = mItor->second.begin();
		for(; itor2 != mItor->second.end(); ++itor2) {
			expr * toAssert = gen_len_val_options_for_free_var(*itor2, NULL, "");
			// same deal with returning a NULL axiom here
			if(toAssert != NULL) {
			    assert_axiom(toAssert);
			}
		}
	}
}

/*
 * Collect all unroll functions
 * and constant string in eqc of node n
 */
void theory_str::get_eqc_allUnroll(expr * n, expr * &constStr, std::set<expr*> & unrollFuncSet) {
    constStr = NULL;
    unrollFuncSet.clear();
    context & ctx = get_context();

    expr * curr = n;
    do {
        if (is_string(to_app(curr))) {
            constStr = curr;
        } else if (is_Unroll(to_app(curr))) {
            if (unrollFuncSet.find(curr) == unrollFuncSet.end()) {
                unrollFuncSet.insert(curr);
            }
        }
        curr = get_eqc_next(curr);
    } while (curr != n);
}

// Collect simple Unroll functions (whose core is Str2Reg) and constant strings in the EQC of n.
void theory_str::get_eqc_simpleUnroll(expr * n, expr * &constStr, std::set<expr*> & unrollFuncSet) {
	constStr = NULL;
	unrollFuncSet.clear();
	context & ctx = get_context();

	expr * curr = n;
	do {
		if (is_string(to_app(curr))) {
			constStr = curr;
		} else if (is_Unroll(to_app(curr))) {
			expr * core = to_app(curr)->get_arg(0);
			if (is_Str2Reg(to_app(core))) {
				if (unrollFuncSet.find(curr) == unrollFuncSet.end()) {
					unrollFuncSet.insert(curr);
				}
			}
		}
		curr = get_eqc_next(curr);
	} while (curr != n);
}

void theory_str::init_model(model_generator & mg) {
    //TRACE("t_str", tout << "initializing model" << std::endl; display(tout););
    m_factory = alloc(str_value_factory, get_manager(), get_family_id());
    mg.register_factory(m_factory);
}

/*
 * Helper function for mk_value().
 * Attempts to resolve the expression 'n' to a string constant.
 * Stronger than get_eqc_value() in that it will perform recursive descent
 * through every subexpression and attempt to resolve those to concrete values as well.
 * Returns the concrete value obtained from this process,
 * guaranteed to satisfy m_strutil.is_string(),
 * if one could be obtained,
 * or else returns NULL if no concrete value was derived.
 */
app * theory_str::mk_value_helper(app * n) {
    if (m_strutil.is_string(n)) {
        return n;
    } else if (is_concat(n)) {
        // recursively call this function on each argument
        SASSERT(n->get_num_args() == 2);
        expr * a0 = n->get_arg(0);
        expr * a1 = n->get_arg(1);

        app * a0_conststr = mk_value_helper(to_app(a0));
        app * a1_conststr = mk_value_helper(to_app(a1));

        if (a0_conststr != NULL && a1_conststr != NULL) {
            const char * a0_str = 0;
            m_strutil.is_string(a0_conststr, &a0_str);

            const char * a1_str = 0;
            m_strutil.is_string(a1_conststr, &a1_str);

            std::string a0_s(a0_str);
            std::string a1_s(a1_str);
            std::string result = a0_s + a1_s;
            return m_strutil.mk_string(result);
        }
    }
    // fallback path
    // try to find some constant string, anything, in the equivalence class of n
    bool hasEqc = false;
    expr * n_eqc = get_eqc_value(n, hasEqc);
    if (hasEqc) {
        return to_app(n_eqc);
    } else {
        return NULL;
    }
}

model_value_proc * theory_str::mk_value(enode * n, model_generator & mg) {
    TRACE("t_str", tout << "mk_value for: " << mk_ismt2_pp(n->get_owner(), get_manager()) <<
                                " (sort " << mk_ismt2_pp(get_manager().get_sort(n->get_owner()), get_manager()) << ")" << std::endl;);
    ast_manager & m = get_manager();
    context & ctx = get_context();
    app_ref owner(m);
    owner = n->get_owner();

    // If the owner is not internalized, it doesn't have an enode associated.
    SASSERT(ctx.e_internalized(owner));

    app * val = mk_value_helper(owner);
    if (val != NULL) {
        return alloc(expr_wrapper_proc, val);
    } else {
        TRACE("t_str", tout << "WARNING: failed to find a concrete value, falling back" << std::endl;);
        // TODO make absolutely sure the reason we can't find a concrete value is because of an unassigned temporary
        // e.g. for an expression like (Concat X $$_str0)
        return alloc(expr_wrapper_proc, m_strutil.mk_string("**UNUSED**"));
    }
}

void theory_str::finalize_model(model_generator & mg) {}

void theory_str::display(std::ostream & out) const {
    out << "TODO: theory_str display" << std::endl;
}

}; /* namespace smt */
