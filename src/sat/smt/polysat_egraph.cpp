/*---
Copyright (c) 2022 Microsoft Corporation

Module Name:

    polysat_egraph.cpp

Abstract:

    PolySAT interface to bit-vector

Author:

    Nikolaj Bjorner (nbjorner) 2022-01-26


--*/

#include "ast/euf/euf_bv_plugin.h"
#include "sat/smt/polysat_solver.h"
#include "sat/smt/euf_solver.h"


namespace polysat {

    // walk the egraph starting with pvar for suffix overlaps.
    void solver::get_bitvector_suffixes(pvar pv, offset_slices& out) {       
        uint_set seen;
        std::function<bool(euf::enode*, unsigned)> consume_slice = [&](euf::enode* n, unsigned offset) {
            if (offset != 0)
                return false;
            for (auto sib : euf::enode_class(n)) {
                auto w = sib->get_th_var(get_id());
                if (w == euf::null_theory_var)
                    continue;
                if (seen.contains(w))
                    continue;
                seen.insert(w);
                auto const& p = m_var2pdd[w];
                if (!p.is_var())
                    continue;
                out.push_back({ p.var(), offset });
            }
            return true;
        };
        theory_var v = m_pddvar2var[pv];
        m_bv_plugin->sub_slices(var2enode(v), consume_slice);
    }

    // walk the egraph starting with pvar for any overlaps.
    void solver::get_bitvector_sub_slices(pvar pv, offset_slices& out) {
        uint_set seen;
        std::function<bool(euf::enode*, unsigned)> consume_slice = [&](euf::enode* n, unsigned offset) {
            for (auto sib : euf::enode_class(n)) {
                auto w = sib->get_th_var(get_id());
                if (w == euf::null_theory_var)
                    continue;
                if (seen.contains(w))
                    continue;
                seen.insert(w);
                auto const& p = m_var2pdd[w];
                if (!p.is_var())
                    continue;
                out.push_back({ p.var(), offset });
            }
            return true;
        };
        theory_var v = m_pddvar2var[pv];
        m_bv_plugin->sub_slices(var2enode(v), consume_slice);
    }

    // walk the egraph for bit-vectors that contain pv.
    void solver::get_bitvector_super_slices(pvar pv, offset_slices& out) {
        uint_set seen;
        std::function<bool(euf::enode*, unsigned)> consume_slice = [&](euf::enode* n, unsigned offset) {
            for (auto sib : euf::enode_class(n)) {
                auto w = sib->get_th_var(get_id());
                if (w == euf::null_theory_var)
                    continue;
                if (seen.contains(w))
                    continue;
                seen.insert(w);
                auto const& p = m_var2pdd[w];
                if (!p.is_var())
                    continue;
                out.push_back({ p.var(), offset });
            }
            return true;
            };
        theory_var v = m_pddvar2var[pv];
        m_bv_plugin->super_slices(var2enode(v), consume_slice);

    }

    // walk the e-graph to retrieve fixed overlaps
    void solver::get_fixed_bits(pvar pv, fixed_bits_vector& out) {

        std::function<bool(euf::enode*, unsigned)> consume_slice = [&](euf::enode* n, unsigned offset) {
            if (!n->interpreted())
                return true;
            auto w = n->get_root()->get_th_var(get_id());
            if (w == euf::null_theory_var)
                return true;
            auto const& p = m_var2pdd[w];
            if (!p.is_var())
                return true;
            unsigned lo = offset, hi = bv.get_bv_size(n->get_expr());
            rational value;
            VERIFY(bv.is_numeral(n->get_expr(), value));
            out.push_back({ fixed_slice(lo, hi, value) });
            return false;
        };
        theory_var v = m_pddvar2var[pv];
        m_bv_plugin->sub_slices(var2enode(v), consume_slice);
    }
    
    void solver::explain_slice(pvar pv, pvar pw, unsigned offset, std::function<void(euf::enode*, euf::enode*)>& consume_eq) {
        euf::theory_var v = m_pddvar2var[pv];
        euf::theory_var w = m_pddvar2var[pw];
        m_bv_plugin->explain_slice(var2enode(v), offset, var2enode(w), consume_eq);
    }

    void solver::explain_fixed(pvar pv, unsigned lo, unsigned hi, rational const& value, std::function<void(euf::enode*, euf::enode*)>& consume_eq) {
        euf::theory_var v = m_pddvar2var[pv];
        expr_ref val(bv.mk_numeral(value, hi - lo + 1), m);
        euf::enode* b = ctx.get_egraph().find(val);
        SASSERT(b);
        m_bv_plugin->explain_slice(var2enode(v), lo, b, consume_eq);
    }

}
