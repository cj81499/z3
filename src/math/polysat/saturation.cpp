/*++
Copyright (c) 2021 Microsoft Corporation

Module Name:

    Polysat core saturation

Author:

    Nikolaj Bjorner (nbjorner) 2021-03-19
    Jakob Rath 2021-04-6


TODO: preserve falsification
- each rule selects a certain premises that are problematic.
  If the problematic premise is false under the current assignment, the newly inferred
  literal should also be false in the assignment in order to preserve conflicts.


TODO: when we check that 'x' is "unary":
- in principle, 'x' could be any polynomial. However, we need to divide the lhs by x, and we don't have general polynomial division yet.
  so for now we just allow the form 'value*variable'.
   (extension to arbitrary monomials for 'x' should be fairly easy too)

--*/
#include "math/polysat/saturation.h"
#include "math/polysat/solver.h"
#include "math/polysat/log.h"
#include "math/polysat/umul_ovfl_constraint.h"

namespace polysat {

    saturation::saturation(solver& s) : s(s), m_lemma(s) {}

    void saturation::perform(pvar v, conflict& core) {
        for (auto c : core) 
            if (perform(v, c, core))
                return;        
    }

    bool saturation::perform(pvar v, signed_constraint const& c, conflict& core) {
        IF_VERBOSE(0, verbose_stream() << v << " " << c << " " << c.is_currently_true(s) << "\n");
        if (!c->is_ule())
            return false;
        if (c.is_currently_true(s))
            return false;
        auto i = inequality::from_ule(c);
        if (try_mul_bounds(v, core, i))
            return true;
        if (try_parity(v, core, i))
            return true;
        if (try_factor_equality(v, core, i))
            return true;
        if (try_ugt_x(v, core, i))
            return true;
        if (try_ugt_y(v, core, i))
            return true;
        if (try_ugt_z(v, core, i))
            return true;
        if (try_y_l_ax_and_x_l_z(v, core, i))
            return true;
        if (try_tangent(v, core, i))
            return true;
        return false;
    }

    signed_constraint saturation::ineq(bool is_strict, pdd const& lhs, pdd const& rhs) {
        if (is_strict)
            return s.ult(lhs, rhs);
        else
            return s.ule(lhs, rhs);
    }

    bool saturation::propagate(conflict& core, inequality const& crit, signed_constraint c) {
        if (is_forced_true(c))
            return false;

        // NSB - review is it enough to propagate a new literal even if it is not false?
        // unit propagation does not require conflicts.
        // it should just avoid redundant propagation on literals that are true
        //
        // Furthermore propagation cannot be used when the resolved variable comes from 
        // forbidden interval conflicts. The propagated literal effectively adds a new and simpler bound
        // on the non-viable variable. This bound then enables tighter non-viability conflicts.
        // Effectively c is forced false, but it is forced false within the context of constraints used for viability.
        //
        // The effective level of the propagation is the level of all the other literals. If their level is below the
        // last decision level (conflict level) we expect the propagation to be useful.
        // The current assumptions on how conflict lemmas are used do not accomodate propagation it seems.
        //

        m_lemma.insert(~crit.as_signed_constraint());

        IF_VERBOSE(10, verbose_stream() << "propagate " << m_rule << " ";
                   for (auto lit : m_lemma) verbose_stream() << s.lit2cnstr(lit) << " ";
                   verbose_stream() << c << "\n";
                   );

        SASSERT(all_of(m_lemma, [this](sat::literal lit) { return is_forced_false(s.lit2cnstr(lit)); }));

        m_lemma.insert(c);
        core.add_lemma(m_rule, m_lemma.build());
        return true;
    }

    bool saturation::add_conflict(conflict& core, inequality const& crit1, signed_constraint c) {
        return add_conflict(core, crit1, crit1, c);
    }

    bool saturation::add_conflict(conflict& core, inequality const& _crit1, inequality const& _crit2, signed_constraint const c) {
        auto crit1 = _crit1.as_signed_constraint();
        auto crit2 = _crit2.as_signed_constraint();
        m_lemma.insert(~crit1);
        if (crit1 != crit2)
            m_lemma.insert(~crit2);

        LOG("critical " << m_rule << " " << crit1);
        LOG("consequent " << c << " value: " << c.bvalue(s) << " is-false: " << c.is_currently_false(s));

        SASSERT(all_of(m_lemma, [this](sat::literal lit) { return s.m_bvars.value(lit) == l_false; }));

        // Ensure lemma is a conflict lemma
        if (!is_forced_false(c)) 
            return false;

        // Constraint c is already on the search stack, so the lemma will not derive anything new.
        if (c.bvalue(s) == l_true)
            return false;

        m_lemma.insert_eval(c);
        core.add_lemma(m_rule, m_lemma.build());
        return true;
    }

    bool saturation::is_non_overflow(pdd const& x, pdd const& y, signed_constraint& c) {
        
        if (is_non_overflow(x, y)) {
            c = ~s.umul_ovfl(x, y);
            return true;
        }

        // TODO: do we really search the stack or can we just create the literal s.umul_ovfl(x, y)
        // and check if it is assigned, or not even create the literal but look up whether it is assigned?
        // constraint_manager uses m_dedup, alloc
        // but to probe whether a literal occurs these are not needed.
        // m_dedup.constraints.contains(&c);
        
        for (auto si : s.m_search) {
            if (!si.is_boolean())
                continue;
            if (si.is_resolved())
                continue;
            auto d = s.lit2cnstr(si.lit());
            if (!d->is_umul_ovfl() || !d.is_negative())
                continue;
            auto const& ovfl = d->to_umul_ovfl();
            if (x != ovfl.p() && x != ovfl.q())
                continue;
            if (y != ovfl.p() && y != ovfl.q())
                continue;
            c = d;
            return true;
        }
        return false;
    }

    /*
     * Match [v] .. <= v
     */
    bool saturation::is_l_v(pvar v, inequality const& i) {
        return i.rhs() == s.var(v);
    }

    /*
     * Match [v] v <= ...
     */
    bool saturation::is_g_v(pvar v, inequality const& i) {
        return i.lhs() == s.var(v);
    }

    /*
     * Match [x] x <= y
     */
    bool saturation::is_x_l_Y(pvar x, inequality const& i, pdd& y) {
        y = i.rhs();
        return is_g_v(x, i);
    }

    /*
     * Match [x] y <= a*x
     */
    bool saturation::is_Y_l_Ax(pvar x, inequality const& i, pdd& a, pdd& y) {
        y = i.lhs();
        return is_xY(x, i.rhs(), a);
    }

    bool saturation::verify_Y_l_Ax(pvar x, inequality const& i, pdd const& a, pdd const& y) {
        return i.lhs() == y && i.rhs() == a * s.var(x);
    }

    /**
     * Match [x] a*x <= y
     */
    bool saturation::is_Ax_l_Y(pvar x, inequality const& i, pdd& a, pdd& y) {
        y = i.rhs();
        return is_xY(x, i.lhs(), a);
    }

    bool saturation::verify_Ax_l_Y(pvar x, inequality const& i, pdd const& a, pdd const& y) {
        return i.rhs() == y && i.lhs() == a * s.var(x);
    }

    /**
     * Match [x] a*x + b <= y
     */
    bool saturation::is_AxB_l_Y(pvar x, inequality const& i, pdd& a, pdd& b, pdd& y) {
        y = i.rhs();
        pdd aa = a, bb = b;
        return i.lhs().degree(x) == 1 && (i.lhs().factor(x, 1, aa, bb), aa == a && bb == b);
    }

    bool saturation::verify_AxB_l_Y(pvar x, inequality const& i, pdd const& a, pdd const& b, pdd const& y) {
        return i.rhs() == y && i.lhs() == a * s.var(x) + b;
    }

    /**
     * Match [x] a*x + b <= y, val(y) = 0
     */
    bool saturation::is_AxB_eq_0(pvar x, inequality const& i, pdd& a, pdd& b, pdd& y) {
        y = i.rhs();
        rational y_val;
        if (!s.try_eval(y, y_val) || y_val != 0)
            return false;
        return i.lhs().degree(x) == 1 && (i.lhs().factor(x, 1, a, b), true);
    }

    bool saturation::verify_AxB_eq_0(pvar x, inequality const& i, pdd const& a, pdd const& b, pdd const& y) {
        return y.is_val() && y.val() == 0 && i.rhs() == y && i.lhs() == a * s.var(x) + b;
    }

    /**
     * Match [coeff*x] coeff*x*Y where x is a variable
     */
    bool saturation::is_coeffxY(pdd const& x, pdd const& p, pdd& y) {
        pdd xy = x.manager().zero();
        return x.is_unary() && p.try_div(x.hi().val(), xy) && xy.factor(x.var(), 1, y);
    }

    /**
     * Determine whether values of x * y is non-overflowing.
     */
    bool saturation::is_non_overflow(pdd const& x, pdd const& y) {
        rational x_val, y_val;
        rational bound = x.manager().two_to_N();
        return s.try_eval(x, x_val) && s.try_eval(y, y_val) && x_val * y_val < bound;
    }

    /**
     * Match [v] v*x <= z*x with x a variable
     */
    bool saturation::is_Xy_l_XZ(pvar v, inequality const& i, pdd& x, pdd& z) {
        return is_xY(v, i.lhs(), x) && is_coeffxY(x, i.rhs(), z);
    }

    bool saturation::verify_Xy_l_XZ(pvar v, inequality const& i, pdd const& x, pdd const& z) {
        return i.lhs() == s.var(v) * x && i.rhs() == z * x;
    }

    /**
     * Match [z] yx <= zx with x a variable
     */
    bool saturation::is_YX_l_zX(pvar z, inequality const& c, pdd& x, pdd& y) {
        return is_xY(z, c.rhs(), x) && is_coeffxY(x, c.lhs(), y);
    }

    bool saturation::verify_YX_l_zX(pvar z, inequality const& c, pdd const& x, pdd const& y) {
        return c.lhs() == y * x && c.rhs() == s.var(z) * x;
    }

    /**
     * Match [x] xY <= xZ
     */
    bool saturation::is_xY_l_xZ(pvar x, inequality const& c, pdd& y, pdd& z) {
        return is_xY(x, c.lhs(), y) && is_xY(x, c.rhs(), z);
    }

    /**
     * Match xy = x * Y
     */
    bool saturation::is_xY(pvar x, pdd const& xy, pdd& y) {
        return xy.degree(x) == 1 && xy.factor(x, 1, y);
    }

    // 
    // overall comment: we use value propagation to check if p is val
    // but we could also use literal propagation and establish there is a literal p = 0 that is true.
    // in this way the value of p doesn't have to be fixed.
    // 
    // is_forced_diseq already creates a literal.
    // is_non_overflow also creates a literal
    // 
    // The condition that p = val may be indirect.
    // it could be a literal
    // it could be by propagation of literals
    // Example:
    //  -35: v90 + v89*v43 + -1*v87 != 0     [ l_false bprop@0 pwatched ]
    //   36: ovfl*(v43, v89)                 [ l_false bprop@0 pwatched ]
    // -218: v90 + -1*v87 + -1 != 0          [ l_false eval@6 pwatched ]
    // 
    // what should we "pay" to establish this condition?
    // or do we just afford us to add this lemma?
    // 

    bool saturation::is_forced_eq(pdd const& p, rational const& val) {
        rational pv;
        if (s.try_eval(p, pv) && pv == val)
            return true;
        return false;
    }

    bool saturation::is_forced_diseq(pdd const& p, int i, signed_constraint& c) {
        c = s.eq(p, i);
        return is_forced_false(c);
    }

    bool saturation::is_forced_odd(pdd const& p, signed_constraint& c) {
        c = s.odd(p);
        return is_forced_true(c);
    }

    bool saturation::is_forced_false(signed_constraint const& c) {
        return c.bvalue(s) == l_false || c.is_currently_false(s);
    }

    bool saturation::is_forced_true(signed_constraint const& c) {
        return c.bvalue(s) == l_true || c.is_currently_true(s);
    }

    /**
     * Implement the inferences
     *  [x] yx < zx   ==>  Ω*(x,y) \/ y < z
     *  [x] yx <= zx  ==>  Ω*(x,y) \/ y <= z \/ x = 0
     */
    bool saturation::try_ugt_x(pvar v, conflict& core, inequality const& xy_l_xz) {
        set_rule("[x] yx <= zx");
        pdd x = s.var(v);
        pdd y = x;
        pdd z = x;
        signed_constraint non_ovfl;

        if (!is_xY_l_xZ(v, xy_l_xz, y, z))
            return false;
        if (!xy_l_xz.is_strict() && s.is_assigned(v) && s.get_value(v).is_zero())
            return false;
        if (!is_non_overflow(x, y, non_ovfl))
            return false;
        m_lemma.reset();
        m_lemma.insert_eval(~non_ovfl);
        if (!xy_l_xz.is_strict())
            m_lemma.insert_eval(s.eq(x));
        return add_conflict(core, xy_l_xz, ineq(xy_l_xz.is_strict(), y, z));
    }

    /**
     * [y] z' <= y /\ yx <= zx  ==>  Ω*(x,y) \/ z'x <= zx
     * [y] z' <= y /\ yx < zx   ==>  Ω*(x,y) \/ z'x < zx
     * [y] z' < y  /\ yx <= zx  ==>  Ω*(x,y) \/ z'x < zx
     * [y] z' < y  /\ yx < zx   ==>  Ω*(x,y) \/ z'x < zx       TODO: could strengthen the conclusion to z'x + 1 < zx
     */
    bool saturation::try_ugt_y(pvar v, conflict& core, inequality const& yx_l_zx) {
        set_rule("[y] z' <= y & yx <= zx");
        auto& m = s.var2pdd(v);
        pdd x = m.zero();
        pdd z = m.zero();
        if (!is_Xy_l_XZ(v, yx_l_zx, x, z))
            return false;
        for (auto si : s.m_search) {
            if (!si.is_boolean())
                continue;
            if (si.is_resolved())
                continue;
            auto d = s.lit2cnstr(si.lit());
            if (!d->is_ule())
                continue;
            auto l_y = inequality::from_ule(d);
            if (is_l_v(v, l_y) && try_ugt_y(v, core, l_y, yx_l_zx, x, z))
                return true;
        }
        return false;
    }

    bool saturation::try_ugt_y(pvar v, conflict& core, inequality const& l_y, inequality const& yx_l_zx, pdd const& x, pdd const& z) {
        SASSERT(is_l_v(v, l_y));
        SASSERT(verify_Xy_l_XZ(v, yx_l_zx, x, z));
        pdd const y = s.var(v);
        signed_constraint non_ovfl;
        if (!is_non_overflow(x, y, non_ovfl))
            return false;
        pdd const& z_prime = l_y.lhs();
        m_lemma.reset();
        m_lemma.insert_eval(~non_ovfl);
        return add_conflict(core, l_y, yx_l_zx, ineq(yx_l_zx.is_strict() || l_y.is_strict(), z_prime * x, z * x));
    }

    /**
     * [z] z <= y' /\ yx <= zx  ==>  Ω*(x,y') \/ yx <= y'x
     * [z] z <= y' /\ yx < zx   ==>  Ω*(x,y') \/ yx < y'x
     * [z] z < y'  /\ yx <= zx  ==>  Ω*(x,y') \/ yx < y'x
     * [z] z < y'  /\ yx < zx   ==>  Ω*(x,y') \/ yx < y'x       TODO: could strengthen the conclusion to yx + 1 < y'x
     */
    bool saturation::try_ugt_z(pvar z, conflict& core, inequality const& yx_l_zx) {
        set_rule("[z] z <= y' && yx <= zx");
        auto& m = s.var2pdd(z);
        pdd y = m.zero();
        pdd x = m.zero();
        if (!is_YX_l_zX(z, yx_l_zx, x, y))
            return false;
        for (auto si : s.m_search) {
            if (!si.is_boolean())
                continue;
            if (si.is_resolved())
                continue;
            auto d = s.lit2cnstr(si.lit());
            if (!d->is_ule())
                continue;
            auto z_l_y = inequality::from_ule(d);
            if (is_g_v(z, z_l_y) && try_ugt_z(z, core, z_l_y, yx_l_zx, x, y))
                return true;
        }
        return false;
    }

    bool saturation::try_ugt_z(pvar z, conflict& core, inequality const& z_l_y, inequality const& yx_l_zx, pdd const& x, pdd const& y) {
        SASSERT(is_g_v(z, z_l_y));
        SASSERT(verify_YX_l_zX(z, yx_l_zx, x, y));
        pdd const& y_prime = z_l_y.rhs();
        signed_constraint non_ovfl;
        if (!is_non_overflow(x, y_prime, non_ovfl))
            return false;
        m_lemma.reset();
        m_lemma.insert_eval(~non_ovfl);
        return add_conflict(core, yx_l_zx, z_l_y, ineq(z_l_y.is_strict() || yx_l_zx.is_strict(), y * x, y_prime * x));
    }

    /**
     * [x]  y <= ax /\ x <= z  (non-overflow case)
     *     ==>   Ω*(a, z)  \/  y <= az
     * ... (other combinations of <, <=)
     */
    bool saturation::try_y_l_ax_and_x_l_z(pvar x, conflict& core, inequality const& y_l_ax) {
        set_rule("[x] y <= ax & x <= z");
        auto& m = s.var2pdd(x);
        pdd y = m.zero();
        pdd a = m.zero();
        if (!is_Y_l_Ax(x, y_l_ax, a, y))
            return false;
        if (a.is_one())
            return false;
        for (auto si : s.m_search) {
            if (!si.is_boolean())
                continue;
            if (si.is_resolved())
                continue;
            auto d = s.lit2cnstr(si.lit());
            if (!d->is_ule())
                continue;
            auto x_l_z = inequality::from_ule(d);
            if (is_g_v(x, x_l_z) && try_y_l_ax_and_x_l_z(x, core, y_l_ax, x_l_z, a, y))
                return true;
        }
        return false;
    }

    bool saturation::try_y_l_ax_and_x_l_z(pvar x, conflict& core, inequality const& y_l_ax, inequality const& x_l_z, pdd const& a, pdd const& y) {
        SASSERT(is_g_v(x, x_l_z));
        SASSERT(verify_Y_l_Ax(x, y_l_ax, a, y));
        pdd const& z = x_l_z.rhs();
        signed_constraint non_ovfl;
        if (!is_non_overflow(a, z, non_ovfl))
            return false;
        m_lemma.reset();
        m_lemma.insert_eval(~non_ovfl);
        return add_conflict(core, y_l_ax, x_l_z, ineq(x_l_z.is_strict() || y_l_ax.is_strict(), y, a * z));
    }

    /**
     * [x] a <= k & a*x + b = 0 & b = 0 => a = 0 or x = 0 or x >= 2^K/k
     * [x] x <= k & a*x + b = 0 & b = 0 => x = 0 or a = 0 or a >= 2^K/k
     * Better?
     * [x] a*x + b = 0 & b = 0 => a = 0 or x = 0 or Ω*(a, x)     
     * We need up to four versions of this for all sign combinations of a, x
     */
    bool saturation::try_mul_bounds(pvar x, conflict& core, inequality const& axb_l_y) {
        set_rule("[x] a*x + b = 0 & b = 0 => a = 0 or x = 0 or ovfl(a, x)");
        auto& m = s.var2pdd(x);
        pdd y = m.zero();
        pdd a = m.zero();
        pdd b = m.zero();
        pdd k = m.zero();
        pdd X = s.var(x);
        rational k_val;
        if (!is_AxB_eq_0(x, axb_l_y, a, b, y))
            return false;
        if (a.is_val())
            return false;        
        if (!is_forced_eq(b, 0))
            return false;

        signed_constraint x_eq_0, a_eq_0;
        if (!is_forced_diseq(X, 0, x_eq_0))
            return false;
        if (!is_forced_diseq(a, 0, a_eq_0))
            return false;

        auto prop1 = [&](signed_constraint c) {
            m_lemma.reset();
            m_lemma.insert_eval(~s.eq(b));
            m_lemma.insert_eval(~s.eq(y));
            m_lemma.insert_eval(x_eq_0);
            m_lemma.insert_eval(a_eq_0);
            return propagate(core, axb_l_y, c);
        };

        auto prop2 = [&](signed_constraint ante, signed_constraint c) {
            m_lemma.reset();
            m_lemma.insert_eval(~s.eq(b));
            m_lemma.insert_eval(~s.eq(y));
            m_lemma.insert_eval(x_eq_0);
            m_lemma.insert_eval(a_eq_0);
            m_lemma.insert_eval(~ante);
            return propagate(core, axb_l_y, c);
        };

        pdd minus_a = -a;
        pdd minus_X = -X;
        pdd Y = X;
        for (auto si : s.m_search) {
            if (!si.is_boolean())
                continue;
            if (si.is_resolved())
                continue;
            auto d = s.lit2cnstr(si.lit());
            if (!d->is_ule())
                continue;
            auto u_l_k = inequality::from_ule(d);
            // a <= k or x <= k
            k = u_l_k.rhs();
            if (!k.is_val())
                continue;
            k_val = k.val();
            if (u_l_k.is_strict())
                k_val -= 1;
            if (k_val <= 1)
                continue;
            if (u_l_k.lhs() == a || u_l_k.lhs() == minus_a) 
                Y = X;
            else if (u_l_k.lhs() == X || u_l_k.lhs() == minus_X) 
                Y = a;
            else
                continue;
            //
            // NSB review: should we handle cases where k_val >= 2^{K-1}, but exploit that x*y = 0 iff -x*y = 0?
            // 
            IF_VERBOSE(0, verbose_stream() << "mult-bounds2 " << Y << " " << axb_l_y.as_signed_constraint() << " " << u_l_k.as_signed_constraint() << " \n");
            rational bound = ceil(rational::power_of_two(m.power_of_2()) / k_val);
            if (prop2(d, s.uge(Y, bound)))
                return true;
            if (prop2(d, s.uge(-Y, bound)))
                return true;                     
        }

        IF_VERBOSE(0, verbose_stream() << "mult-bounds1 " << a << " " << axb_l_y.as_signed_constraint() << " \n");
        IF_VERBOSE(0, verbose_stream() << core << "\n"); 
        if (prop1(s.umul_ovfl(a, X)))
            return true;
        if (prop1(s.umul_ovfl(a, -X)))
            return true;
        if (prop1(s.umul_ovfl(-a, X)))
            return true;
        if (prop1(s.umul_ovfl(-a, -X)))
            return true;

        return false;
    }


    /*
     * x*y = 1 & ~ovfl(x,y) => x = 1 
     * x*y = -1 & ~ovfl(-x,y) => -x = 1 
     */
    bool saturation::try_mul_eq_1(pvar x, conflict& core, inequality const& axb_l_y) {
        set_rule("[x] ax + b <= y & y = 0 & b = -1 & ~ovfl(a,x) => x = 1");
        auto& m = s.var2pdd(x);
        pdd y = m.zero();
        pdd a = m.zero();
        pdd b = m.zero();
        pdd X = s.var(x);
        signed_constraint non_ovfl;
        if (!is_AxB_eq_0(x, axb_l_y, a, b, y))
            return false;
        if (!is_forced_eq(b, -1))
            return false;
        if (!is_non_overflow(a, X, non_ovfl)) 
            return false;
        m_lemma.reset();
        m_lemma.insert_eval(~s.eq(b, rational(-1)));
        m_lemma.insert_eval(~s.eq(y));
        m_lemma.insert_eval(~non_ovfl);
        if (propagate(core, axb_l_y, s.eq(X, 1)))
            return true;
        if (propagate(core, axb_l_y, s.eq(a, 1)))
            return true;
        return false;
    }

    /**
     * odd(x*y) => odd(x) 
     * even(x) => even(x*y)
     *
     * parity(x) <= parity(x*y)
     * parity(x) = k & parity(x*y) = k + j => parity(y) = j
     * parity(x) = k & parity(y) = j => parity(x*y) = k + j
     *
     * odd(x) & even(y) => x + y != 0
     *
     * General rule:
     * 
     * a*x + y = 0 => min(K, parity(a) + parity(x)) = parity(y)
     *
     * Currently implemented special case: a*x + y = 0 => (odd(b) <=> odd(a) & odd(x))
     *
     * general rule can be obtained by adding an
     * 'is_forced_parity(x, p, x_has_parity_p)'
     * 
     * Should we also check 'is_forced_parity(a*x, p, ax_has_parity_p)'
     * if a*x has a parity but not a, x?
     * 
     */
    bool saturation::try_parity(pvar x, conflict& core, inequality const& axb_l_y) {
        set_rule("[x] a*x + b = 0 => (odd(a) & odd(x) <=> odd(b))");

        IF_VERBOSE(0, verbose_stream() << "try parity " << axb_l_y.as_signed_constraint() << "\n");
        auto& m = s.var2pdd(x);
        unsigned N = m.power_of_2();
        pdd y = m.zero();
        pdd a = m.zero();
        pdd b = m.zero();
        pdd X = s.var(x);
        if (!is_AxB_eq_0(x, axb_l_y, a, b, y))
            return false;
        if (a.is_max() && b.is_var())  // x == y, we propagate values in each direction and don't need a lemma
            return false;
        if (a.is_one() && (-b).is_var())  // y == x
            return false;
        signed_constraint b_is_odd = s.odd(b);
        signed_constraint a_is_odd = s.odd(a);
        signed_constraint x_is_odd = s.odd(X);

        auto propagate1 = [&](signed_constraint premise, signed_constraint conseq) {
            m_lemma.reset();
            m_lemma.insert_eval(~s.eq(y));
            m_lemma.insert_eval(~premise);
            return propagate(core, axb_l_y, conseq);
        };

        auto propagate2 = [&](signed_constraint premise1, signed_constraint premise2, signed_constraint conseq) {
            m_lemma.reset();
            m_lemma.insert_eval(~s.eq(y));
            m_lemma.insert_eval(~premise1);
            m_lemma.insert_eval(~premise2);
            return propagate(core, axb_l_y, conseq);
        };
#if 0
        LOG_H1("try_parity: " << X << " on: " << lit_pp(s, axb_l_y.as_signed_constraint()));
        LOG("y: " << y << "   a: " << a << "   b: " << b);
        LOG("b_is_odd: " << lit_pp(s, b_is_odd));
        LOG("a_is_odd: " << lit_pp(s, a_is_odd));
        LOG("x_is_odd: " << lit_pp(s, x_is_odd));
#endif
        if (a_is_odd.is_currently_true(s) &&
            x_is_odd.is_currently_true(s) &&
            propagate2(a_is_odd, x_is_odd, b_is_odd))
            return true;


        if (b_is_odd.is_currently_true(s)) {
            if (propagate1(b_is_odd, a_is_odd))
                return true;
            if (propagate1(b_is_odd, x_is_odd))
                return true;
        }
        
        // a is divisibly by 4,
        // max divisor of x is k
        // -> b has parity k + 4
        unsigned a_parity = a_is_odd.is_currently_false(s) ? 1 : 0;
        unsigned x_parity = x_is_odd.is_currently_false(s) ? 1 : 0;

        if ((a_parity > 0 || x_parity > 0) && !is_forced_eq(a, 0) && !is_forced_eq(X, 0)) {          
            while (a_parity < N && s.parity(a, a_parity+1).is_currently_true(s))
                ++a_parity;
            while (x_parity < N && s.parity(X, x_parity+1).is_currently_true(s))
                ++x_parity;
            unsigned b_parity = std::min(N, a_parity + x_parity);
            if (a_parity > 0 && x_parity > 0 && propagate2(s.parity(a, a_parity), s.parity(X, x_parity), s.parity(b, b_parity)))
                return true;
            if (a_parity > 0 && x_parity == 0 && propagate1(s.parity(a, a_parity), s.parity(b, b_parity)))
                return true;
            if (a_parity == 0 && x_parity > 0 && propagate1(s.parity(X, x_parity), s.parity(b, b_parity)))
                return true;
        }

        // 
        // if b has at most b_parity, then a*x has at most b_parity
        // 
        else if (!is_forced_eq(b, 0)) {
            unsigned b_parity = 1;
            bool found = false;
            for (; b_parity < N; ++b_parity) {
                if (s.parity(b, b_parity).is_currently_false(s)) {
                    found = true;
                    break;
                }
            }
            if (found) {
                if (propagate1(~s.parity(b, b_parity), ~s.parity(a, b_parity)))
                    return true;
                if (propagate1(~s.parity(b, b_parity), ~s.parity(a, b_parity)))
                    return true;

                for (unsigned i = 1; i < N; ++i) {
                    if (s.parity(a, i).is_currently_true(s) && 
                        propagate2(~s.parity(b, b_parity), s.parity(a, i), ~s.parity(X, b_parity - i)))
                        return true;

                    if (s.parity(X, i).is_currently_true(s) && 
                        propagate2(~s.parity(b, b_parity), s.parity(X, i), ~s.parity(a, b_parity - i)))
                        return true;                    
                }
            }
        }
        return false;        
    }

    /**
     * a*x = 0 => a = 0 or even(x)
     * a*x = 0 => a = 0 or x = 0 or even(a)
     */
    bool saturation::try_mul_odd(pvar x, conflict& core, inequality const& axb_l_y) {
        set_rule("[x] ax = 0 => a = 0 or even(x)");
        auto& m = s.var2pdd(x);
        pdd y = m.zero();
        pdd a = m.zero();
        pdd b = m.zero();
        pdd X = s.var(x);
        signed_constraint a_eq_0, x_eq_0;
        if (!is_AxB_eq_0(x, axb_l_y, a, b, y))
            return false;
        if (!is_forced_eq(b, 0))
            return false;
        if (!is_forced_diseq(a, 0, a_eq_0))
            return false;
        m_lemma.reset();
        m_lemma.insert_eval(s.eq(y));
        m_lemma.insert_eval(~s.eq(b));
        m_lemma.insert_eval(a_eq_0);
        if (propagate(core, axb_l_y, s.even(X)))
            return true;
        if (!is_forced_diseq(X, 0, x_eq_0))
            return false;
        m_lemma.insert_eval(x_eq_0);
        if (propagate(core, axb_l_y, s.even(a)))
            return true;
        return false;
    }

    /**
     *  [x] ax + p <= q, ax + r = 0 => -r + p <= q
     *  [x] p <= ax + q, ax + r = 0 => p <= -r + q
     *  generalizations
     *  [x] abx + p <= q, ax + r = 0 => -rb + p <= q
     *  [x] p <= abx + q, ax + r = 0 => p <= -rb + q
     */

    bool saturation::try_factor_equality(pvar x, conflict& core, inequality const& a_l_b) {
        // search for abx+p pattern in a_l_b.lhs()
        // search for ax + r = 0 equality in core (or in search but maybe not needed)
        // replace abx by -rb in patterns on either a_l_b.lhs() or a_l_b.rhs() or both if available to form new implied
        // literal wtihout occurrence of x
        return false;
    }
    /*
     * TODO
     *
     * Maybe also
     * x*y = k => \/_{j is such that there is j', j*j' = k} x = j
     * x*y = k & ~ovfl(x,y) & x = j => y = k/j where j is a divisor of k
     */


    /**
     * [x] p(x) <= q(x) where value(p) > value(q)
     *     ==> q <= value(q) => p <= value(q)
     *
     * for strict?
     *     p(x) < q(x) where value(p) >= value(q)
     *     ==> value(p) <= p => value(p) < q
     */
    bool saturation::try_tangent(pvar v, conflict& core, inequality const& c) {
        set_rule("[x] p(x) <= q(x) where value(p) > value(q)");
        // if (c.is_strict())
        //     return false;
        if (!c.as_signed_constraint()->contains_var(v))
            return false;
        if (c.lhs().is_val() || c.rhs().is_val())
            return false;

        auto& m = s.var2pdd(v);
        pdd q_l(m), e_l(m), q_r(m), e_r(m);
        bool is_linear = true;
        is_linear &= c.lhs().degree(v) <= 1;
        is_linear &= c.rhs().degree(v) <= 1;
        if (c.lhs().degree(v) == 1) {
            c.lhs().factor(v, 1, q_l, e_l);
            is_linear &= q_l.is_val();
        }
        if (c.rhs().degree(v) == 1) {
            c.rhs().factor(v, 1, q_r, e_r);
            is_linear &= q_r.is_val();
        }
        if (is_linear)
            return false;

        if (!c.as_signed_constraint().is_currently_false(s))
            return false;
        rational l_val, r_val;
        if (!s.try_eval(c.lhs(), l_val))
            return false;
        if (!s.try_eval(c.rhs(), r_val))
            return false;
        SASSERT(c.is_strict() || l_val > r_val);
        SASSERT(!c.is_strict() || l_val >= r_val);
        m_lemma.reset();
        if (c.is_strict()) {
            auto d = s.ule(l_val, c.lhs());
            if (d.bvalue(s) == l_false) // it is a different value conflict that contains v
                return false;
            m_lemma.insert_eval(~d);
            auto conseq = s.ult(r_val, c.rhs());
            return add_conflict(core, c, conseq);
        }
        else {
            auto d = s.ule(c.rhs(), r_val);
            if (d.bvalue(s) == l_false) // it is a different value conflict that contains v
                return false;
            m_lemma.insert_eval(~d);
            auto conseq = s.ule(c.lhs(), r_val);
            return add_conflict(core, c, conseq);
        }
    }

}
