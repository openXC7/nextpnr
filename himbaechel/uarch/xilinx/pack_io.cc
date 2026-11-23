/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019-23  Myrtle Shah <gatecat@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <algorithm>
#include <boost/algorithm/string.hpp>
#include <queue>
#include <unordered_set>
#include "design_utils.h"
#include "extra_data.h"
#include "log.h"
#include "nextpnr.h"
#include "pack.h"
#include "pins.h"

#define HIMBAECHEL_CONSTIDS "uarch/xilinx/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

CellInfo *XilinxPacker::insert_obuf(IdString name, IdString type, NetInfo *i, NetInfo *o, NetInfo *tri)
{
    auto obuf = create_cell(type, name);
    obuf->connectPort(id_I, i);
    obuf->connectPort(id_T, tri);
    obuf->connectPort(id_O, o);
    return obuf;
}

CellInfo *XilinxPacker::insert_outinv(IdString name, NetInfo *i, NetInfo *o)
{
    auto inv = create_cell(id_INV, name);
    inv->connectPort(id_I, i);
    inv->connectPort(id_O, o);
    return inv;
}

CellInfo *XC7Packer::insert_ibuf(IdString name, IdString type, NetInfo *i, NetInfo *o)
{
    auto inbuf = create_cell(type, name);
    inbuf->connectPort(id_I, i);
    inbuf->connectPort(id_O, o);
    return inbuf;
}

CellInfo *XC7Packer::insert_diffibuf(IdString name, IdString type, const std::array<NetInfo *, 2> &i, NetInfo *o)
{
    auto inbuf = create_cell(type, name);
    inbuf->connectPort(id_I, i[0]);
    inbuf->connectPort(id_IB, i[1]);
    inbuf->connectPort(id_O, o);
    return inbuf;
}

NetInfo *XilinxPacker::invert_net(NetInfo *toinv)
{
    if (toinv == nullptr)
        return nullptr;
    // If net is driven by an inverter, don't double-invert, which could cause problems with timing
    // and IOLOGIC packing
    if (toinv->driver.cell != nullptr && toinv->driver.cell->type == id_LUT1 &&
        int_or_default(toinv->driver.cell->params, id_INIT, 0) == 1) {
        NetInfo *preinv = toinv->driver.cell->getPort(id_I0);
        // If only one user, also sweep the inversion LUT to avoid packing issues
        if (toinv->users.entries() == 1) {
            packed_cells.insert(toinv->driver.cell->name);
            toinv->driver.cell->disconnectPort(id_I0);
            toinv->driver.cell->disconnectPort(id_O);
        }
        return preinv;
    } else {
        NetInfo *inv = ctx->createNet(ctx->idf("%s$inverted%d", toinv->name.c_str(ctx), autoidx++));
        create_lut(inv->name.str(ctx) + "$lut", {toinv}, inv, Property(1));
        return inv;
    }
}

std::pair<CellInfo *, PortRef> XilinxPacker::insert_pad_and_buf(CellInfo *npnr_io)
{
    // Given a nextpnr IO buffer, create a PAD instance and insert an IO buffer if one isn't already present
    std::pair<CellInfo *, PortRef> result;
    std::unique_ptr<CellInfo> pad_cell = std::make_unique<CellInfo>(ctx, npnr_io->name, id_PAD);
    pad_cell->addInout(id_PAD);
    // Copy IO attributes to pad
    for (auto &attr : npnr_io->attrs)
        pad_cell->attrs[attr.first] = attr.second;
    NetInfo *ionet = nullptr;
    PortRef iobuf;
    iobuf.cell = nullptr;
    if (npnr_io->type == ctx->id("$nextpnr_obuf") || npnr_io->type == ctx->id("$nextpnr_iobuf")) {
        ionet = npnr_io->getPort(id_I);
        if (ionet != nullptr && ionet->driver.cell != nullptr)
            if (toplevel_ports.count(ionet->driver.cell->type) &&
                toplevel_ports.at(ionet->driver.cell->type).count(ionet->driver.port)) {
                if (ionet->users.entries() > 1)
                    log_error("IO buffer '%s' is connected to more than a single top level IO pin.\n",
                              ionet->driver.cell->name.c_str(ctx));
                iobuf = ionet->driver;
            }
        pad_cell->attrs[id_X_IO_DIR] = std::string(npnr_io->type == ctx->id("$nextpnr_obuf") ? "OUT" : "INOUT");
    }
    if (npnr_io->type == ctx->id("$nextpnr_ibuf") || npnr_io->type == ctx->id("$nextpnr_iobuf")) {
        ionet = npnr_io->getPort(id_O);
        if (ionet != nullptr)
            for (auto &usr : ionet->users)
                if (toplevel_ports.count(usr.cell->type) && toplevel_ports.at(usr.cell->type).count(usr.port)) {
                    if (ionet->users.entries() > 1)
                        log_error("IO buffer '%s' is connected to more than a single top level IO pin.\n",
                                  usr.cell->name.c_str(ctx));
                    iobuf = usr;
                }
        pad_cell->attrs[id_X_IO_DIR] = std::string(npnr_io->type == ctx->id("$nextpnr_ibuf") ? "IN" : "INOUT");
    }

    if (!iobuf.cell) {
        // No IO buffer, need to create one
        log_error(
                "   IO port '%s' is missing an IO buffer, do you need to remove -noiopad from your Yosys arguments?\n",
                npnr_io->name.c_str(ctx));
    } else {
        log_info("    IO port '%s' driven by %s '%s'\n", npnr_io->name.c_str(ctx), iobuf.cell->type.c_str(ctx),
                 iobuf.cell->name.c_str(ctx));
    }

    NPNR_ASSERT(ionet != nullptr);

    for (auto &port : npnr_io->ports)
        npnr_io->disconnectPort(port.first);

    pad_cell->connectPort(id_PAD, ionet);
    if (iobuf.cell->ports.at(iobuf.port).net != ionet) {
        iobuf.cell->disconnectPort(iobuf.port);
        iobuf.cell->connectPort(iobuf.port, ionet);
    }

    result.first = pad_cell.get();
    result.second = iobuf;
    // delete the npnr io and then add the pad, to avoid a name conflict
    for (auto &port : npnr_io->ports) {
        npnr_io->disconnectPort(port.first);
    }
    ctx->cells.erase(npnr_io->name);

    ctx->cells[result.first->name] = std::move(pad_cell);

    return result;
}

void XC7Packer::decompose_iob(CellInfo *xil_iob, bool is_hr, const std::string &iostandard)
{
    bool is_se_ibuf = xil_iob->type.in(id_IBUF, id_IBUF_IBUFDISABLE, id_IBUF_INTERMDISABLE);
    bool is_se_iobuf = xil_iob->type.in(id_IOBUF, id_IOBUF_DCIEN, id_IOBUF_INTERMDISABLE);
    bool is_se_obuf = xil_iob->type.in(id_OBUF, id_OBUFT);

    auto pad_site = [&](NetInfo *n) {
        for (auto user : n->users)
            if (user.cell->type == id_PAD)
                return uarch->get_bel_site(ctx->getBelByNameStr(user.cell->attrs[id_BEL].as_string()));
        NPNR_ASSERT_FALSE(("can't find PAD for net " + n->name.str(ctx)).c_str());
    };

    if (is_se_ibuf || is_se_iobuf) {
        log_info("Generating input buffer for '%s'\n", xil_iob->name.c_str(ctx));
        NetInfo *pad_net = xil_iob->getPort(is_se_iobuf ? id_IO : id_I);
        NPNR_ASSERT(pad_net != nullptr);
        auto site = pad_site(pad_net);
        if (!is_se_iobuf)
            xil_iob->disconnectPort(id_I);

        NetInfo *top_out = xil_iob->getPort(id_O);
        xil_iob->disconnectPort(id_O);

        IdString ibuf_type = id_IBUF;
        if (xil_iob->type.in(id_IBUF_IBUFDISABLE, id_IOBUF_DCIEN))
            ibuf_type = id_IBUF_IBUFDISABLE;
        if (xil_iob->type.in(id_IBUF_INTERMDISABLE, id_IOBUF_INTERMDISABLE))
            ibuf_type = id_IBUF_INTERMDISABLE;

        CellInfo *inbuf = insert_ibuf(int_name(xil_iob->name, "IBUF", is_se_iobuf), ibuf_type, pad_net, top_out);
        std::string tile = ctx->get_tile_type(site.tile).str(ctx);
        bool is_hp_bank = boost::contains(tile, "IOB18");
        if (is_hp_bank)
            ctx->bindBel(uarch->get_site_bel(site, ctx->id("IOB18.INBUF_DCIEN")), inbuf, STRENGTH_LOCKED);
        else
            ctx->bindBel(uarch->get_site_bel(site, ctx->id("IOB33.INBUF_EN")), inbuf, STRENGTH_LOCKED);
        xil_iob->movePortTo(id_IBUFDISABLE, inbuf, id_IBUFDISABLE);
        xil_iob->movePortTo(id_INTERMDISABLE, inbuf, id_INTERMDISABLE);
    }

    if (is_se_obuf || is_se_iobuf) {
        log_info("Generating output buffer for '%s'\n", xil_iob->name.c_str(ctx));
        NetInfo *pad_net = xil_iob->getPort(is_se_iobuf ? id_IO : id_O);
        NPNR_ASSERT(pad_net != nullptr);
        auto site = pad_site(pad_net);
        xil_iob->disconnectPort(is_se_iobuf ? id_IO : id_O);
        bool has_dci = xil_iob->type == id_IOBUF_DCIEN;
        CellInfo *obuf = insert_obuf(
                int_name(xil_iob->name, (is_se_iobuf || xil_iob->type == id_OBUFT) ? "OBUFT" : "OBUF", !is_se_obuf),
                is_se_iobuf ? (has_dci ? id_OBUFT_DCIEN : id_OBUFT) : xil_iob->type, xil_iob->getPort(id_I), pad_net,
                xil_iob->getPort(id_T));
        std::string tile = ctx->get_tile_type(site.tile).str(ctx);
        bool is_hp_bank = boost::contains(tile, "IOB18");
        if (is_hp_bank)
            ctx->bindBel(uarch->get_site_bel(site, ctx->id("IOB18.OUTBUF_DCIEN")), obuf, STRENGTH_LOCKED);
        else
            ctx->bindBel(uarch->get_site_bel(site, ctx->id("IOB33.OUTBUF")), obuf, STRENGTH_LOCKED);
        xil_iob->movePortTo(id_DCITERMDISABLE, obuf, id_DCITERMDISABLE);
    }

    bool is_diff_ibuf = xil_iob->type.in(id_IBUFDS, id_IBUFGDS, id_IBUFDS_INTERMDISABLE, id_IBUFDS);
    // Complementary output OB is the N-side input buffer of the same pair.
    // These three are not IBUFDS: packing only the master leaves OB undriven
    // and the flow used to exit 0 with no input buffer in the bitstream.
    const bool is_diff_out_ibuf =
            xil_iob->type.in(id_IBUFDS_DIFF_OUT, id_IBUFDS_DIFF_OUT_IBUFDISABLE, id_IBUFDS_DIFF_OUT_INTERMDISABLE);
    bool is_diff_iobuf = xil_iob->type.in(id_IOBUFDS, id_IOBUFDS_DCIEN);
    bool is_diff_obuf = xil_iob->type.in(id_OBUFDS, id_OBUFTDS);

    const bool packs_diff_input = is_diff_ibuf || is_diff_out_ibuf || is_diff_iobuf;
    if (packs_diff_input) {
        NetInfo *pad_p_net = xil_iob->getPort(is_diff_iobuf ? id_IO : id_I);
        NPNR_ASSERT(pad_p_net != nullptr);
        auto site_p = pad_site(pad_p_net);
        NetInfo *pad_n_net = xil_iob->getPort(is_diff_iobuf ? id_IOB : id_IB);
        NPNR_ASSERT(pad_n_net != nullptr);
        std::string tile_p = ctx->get_tile_type(site_p.tile).str(ctx);
        bool is_iob18 = boost::contains(tile_p, "IOB18");

        const bool disconnect_diff_inputs = !is_diff_iobuf;
        if (disconnect_diff_inputs) {
            xil_iob->disconnectPort(id_I);
            xil_iob->disconnectPort(id_IB);
        }

        NetInfo *top_out = xil_iob->getPort(id_O);
        xil_iob->disconnectPort(id_O);

        // Stay on IBUFDS for every DIFF_OUT variant. That rule maps IB to
        // DIFFI_IN. IBUFDS_INTERMDISABLE_INT's rule does not, so the N pad
        // would not reach the differential input. IBUFDISABLE and
        // INTERMDISABLE are bel ports of those names and survive the IBUFDS
        // transform. There is no constid for an IBUFDISABLE-only diff cell.
        const IdString ibuf_type = id_IBUFDS;
        CellInfo *inbuf = insert_diffibuf(int_name(xil_iob->name, "IBUF", is_se_iobuf), ibuf_type,
                                          {pad_p_net, pad_n_net}, top_out);
        if (is_iob18) {
            ctx->bindBel(uarch->get_site_bel(site_p, ctx->id("IOB18M.INBUF_DCIEN")), inbuf, STRENGTH_LOCKED);
            inbuf->attrs[id_X_IOB_SITE_TYPE] = std::string("IOB18M");
        } else {
            ctx->bindBel(uarch->get_site_bel(site_p, ctx->id("IOB33M.INBUF_EN")), inbuf, STRENGTH_LOCKED);
            inbuf->attrs[id_X_IOB_SITE_TYPE] = std::string("IOB33M");
        }

        // OB is a second differential receiver on the N site, with I and IB
        // swapped against the master. Vivado expands IBUFDS_DIFF_OUT that way
        // (master IOB33M.INBUF_EN I=P IB=N, slave IOB33S.INBUF_EN I=N IB=P)
        // and the complement leaves the tile on IOB_IBUF1. A single-ended
        // buffer on the N pad is not that receiver. The bel names below are
        // created only on this path: an unconditional id would shift every
        // later id and move place and route of every design.
        const bool has_complement = is_diff_out_ibuf;
        if (has_complement) {
            // IOB18 INBUF_DCIEN has IBUFDISABLE and no INTERMDISABLE port.
            // Dropping the port would build a buffer that ignores it.
            const bool hp_interm_unsupported = is_iob18 && xil_iob->type == id_IBUFDS_DIFF_OUT_INTERMDISABLE;
            if (hp_interm_unsupported)
                log_error("%s '%s' has no packer on a high-performance bank: the IOB18 input buffer has no "
                          "INTERMDISABLE port.\n",
                          xil_iob->type.c_str(ctx), ctx->nameOf(xil_iob));
            NetInfo *top_ob = xil_iob->getPort(id_OB);
            xil_iob->disconnectPort(id_OB);
            const bool complement_drives = top_ob != nullptr && top_ob->users.entries() > 0;
            log_info("Generating complementary input buffer for '%s'\n", xil_iob->name.c_str(ctx));
            auto site_n = pad_site(pad_n_net);
            const bool same_tile = site_p.tile == site_n.tile;
            IdString slave_bel_name;
            const bool n_side_is_hp = is_iob18;
            if (n_side_is_hp)
                slave_bel_name = ctx->id("IOB18S.INBUF_DCIEN");
            else
                slave_bel_name = ctx->id("IOB33S.INBUF_EN");
            BelId slave_bel = uarch->get_site_bel(site_n, slave_bel_name);
            const bool slave_inbuf_present = slave_bel != BelId();
            const bool complement_placeable = same_tile && slave_inbuf_present;
            const bool complement_unplaceable = !complement_placeable;
            if (complement_unplaceable)
                log_error("%s '%s' cannot be placed: OB is the input buffer of the N pin, and %s.\n"
                          "       Constrain I to an IO_L<n>P_... pin and IB to the matching IO_L<n>N_... pin\n"
                          "       of the same pair (see package_pins.csv for the part).\n",
                          xil_iob->type.c_str(ctx), ctx->nameOf(xil_iob),
                          !same_tile ? "the two pins are not the two halves of one differential pair"
                                     : "its N side is constrained to a P (master) pin");
            // I = N pad, IB = P pad. insert_diffibuf takes {I, IB}.
            // An unused OB still has the N-site receiver; its OUT stays open,
            // which is how Vivado leaves the cell when nothing reads OB.
            NetInfo *slave_out = complement_drives ? top_ob : nullptr;
            CellInfo *slave = insert_diffibuf(int_name(xil_iob->name, "IBUF_N", true), ibuf_type,
                                              {pad_n_net, pad_p_net}, slave_out);
            ctx->bindBel(slave_bel, slave, STRENGTH_LOCKED);
            const bool slave_attr_is_hp = is_iob18;
            if (slave_attr_is_hp)
                slave->attrs[id_X_IOB_SITE_TYPE] = std::string("IOB18S");
            else
                slave->attrs[id_X_IOB_SITE_TYPE] = std::string("IOB33S");
            // Vivado ties IBUFDISABLE and INTERMDISABLE to both halves.
            // One half alone leaves IBUFDISABLE.I / INTERMDISABLE.I off the other.
            xil_iob->movePortTo(id_IBUFDISABLE, inbuf, id_IBUFDISABLE);
            xil_iob->movePortTo(id_INTERMDISABLE, inbuf, id_INTERMDISABLE);
            auto share_input = [&](IdString port) {
                NetInfo *net = inbuf->getPort(port);
                const bool no_net = net == nullptr;
                if (no_net)
                    return;
                const bool slave_missing_port = !slave->ports.count(port);
                if (slave_missing_port) {
                    slave->ports[port].name = port;
                    slave->ports[port].type = PORT_IN;
                }
                slave->connectPort(port, net);
            };
            share_input(id_IBUFDISABLE);
            share_input(id_INTERMDISABLE);
        }
    }

    const bool packs_diff_output = is_diff_obuf || is_diff_iobuf;
    if (packs_diff_output) {
        // FIXME: true diff outputs
        NetInfo *pad_p_net = xil_iob->getPort(is_diff_iobuf ? id_IO : id_O);
        NPNR_ASSERT(pad_p_net != nullptr);
        auto site_p = pad_site(pad_p_net);
        NetInfo *pad_n_net = xil_iob->getPort(is_diff_iobuf ? id_IOB : id_OB);
        NPNR_ASSERT(pad_n_net != nullptr);
        auto site_n = pad_site(pad_n_net);
        std::string tile_p = ctx->get_tile_type(site_p.tile).str(ctx);
        bool is_iob18 = boost::contains(tile_p, "IOB18");

        // Name a pad the way the user wrote it, so a placement complaint can be traced
        // back to a line of XDC rather than to a site number. (port of nextpnr-xilinx
        // 0ebf6394)
        auto pad_desc = [&](NetInfo *n, SiteIndex site) {
            for (auto user : n->users) {
                bool is_pad_cell = user.cell->type == id_PAD;
                if (!is_pad_cell)
                    continue;
                std::string d = "'" + user.cell->name.str(ctx) + "'";
                bool has_package_pin = user.cell->attrs.count(id_PACKAGE_PIN);
                bool has_loc = user.cell->attrs.count(id_LOC);
                if (has_package_pin)
                    d += " (package pin " + user.cell->attrs.at(id_PACKAGE_PIN).as_string() + ")";
                else if (has_loc)
                    d += " (package pin " + user.cell->attrs.at(id_LOC).as_string() + ")";
                return d + " at site " + uarch->get_site_name(site).str(ctx);
            }
            return std::string("<unknown pad>");
        };

        // A differential pair is the master (P) and slave (N) site of one IOB tile, and
        // only the master site carries the M bels.  Constraining P to an N pin, or
        // splitting the pair across two tiles, therefore failed much later as an
        // uncaught std::out_of_range out of bindBel -- no pin names, no hint of the
        // swap.  Check it up front and say what is wrong.
        //
        // Each site is classified by its OWN tile type: a split pair can put P in an
        // IOB33 tile and N in an IOB18 one, and judging N by P's tile type would look
        // for an IOB33S bel in an IOB18 site and misreport a valid slave as a master.
        auto site_is_iob18 = [&](SiteIndex site) {
            return boost::contains(ctx->get_tile_type(site.tile).str(ctx), "IOB18");
        };
        auto check_diff_pair = [&](NetInfo *p_net, NetInfo *n_net, SiteIndex p_site, SiteIndex n_site) {
            IdString m_bel = site_is_iob18(p_site) ? ctx->id("IOB18M.OUTBUF_DCIEN") : ctx->id("IOB33M.OUTBUF");
            IdString s_bel = site_is_iob18(n_site) ? ctx->id("IOB18S.O_ININV") : ctx->id("IOB33S.O_ININV");
            bool p_is_master = uarch->get_site_bel(p_site, m_bel) != BelId();
            bool n_is_slave = uarch->get_site_bel(n_site, s_bel) != BelId();
            bool same_tile = p_site.tile == n_site.tile;
            bool is_valid_pair = p_is_master && n_is_slave && same_tile;
            if (is_valid_pair)
                return;
            const char *why = !p_is_master ? "its P side is constrained to the N (slave) pin of a pair"
                            : !n_is_slave ? "its N side is constrained to a P (master) pin"
                                          : "the two pins are not the two halves of one differential pair";
            log_error("%s '%s' cannot be placed: %s.\n"
                      "       P side: %s\n"
                      "       N side: %s\n"
                      "       Constrain the P port to an IO_L<n>P_... pin and the N port to the matching\n"
                      "       IO_L<n>N_... pin of the same pair (see package_pins.csv for the part).\n",
                      xil_iob->type.c_str(ctx), ctx->nameOf(xil_iob), why,
                      pad_desc(p_net, p_site).c_str(), pad_desc(n_net, n_site).c_str());
        };

        check_diff_pair(pad_p_net, pad_n_net, site_p, site_n);

        xil_iob->disconnectPort(is_diff_iobuf ? id_IO : id_O);
        xil_iob->disconnectPort(is_diff_iobuf ? id_IOB : id_OB);

        NetInfo *inv_i = create_internal_net(xil_iob->name, is_diff_obuf ? "I_B" : "OBUFTDS$subnet$I_B");
        CellInfo *inv = insert_outinv(int_name(xil_iob->name, is_diff_obuf ? "INV" : "OBUFTDS$subcell$INV"),
                                      xil_iob->getPort(id_I), inv_i);
        if (is_iob18) {
            ctx->bindBel(uarch->get_site_bel(site_n, ctx->id("IOB18S.O_ININV")), inv, STRENGTH_LOCKED);
            inv->attrs[id_X_IOB_SITE_TYPE] = std::string("IOB18S");
        } else {
            ctx->bindBel(uarch->get_site_bel(site_n, ctx->id("IOB33S.O_ININV")), inv, STRENGTH_LOCKED);
            inv->attrs[id_X_IOB_SITE_TYPE] = std::string("IOB33S");
        }

        bool has_dci = xil_iob->type.in(id_IOBUFDS_DCIEN, id_IOBUFDSE3);

        CellInfo *obuf_p = insert_obuf(
                int_name(xil_iob->name, is_diff_obuf ? "P" : "OBUFTDS$subcell$P"),
                (is_diff_iobuf || (xil_iob->type == id_OBUFTDS)) ? (has_dci ? id_OBUFT_DCIEN : id_OBUFT) : id_OBUF,
                xil_iob->getPort(id_I), pad_p_net, xil_iob->getPort(id_T));

        if (is_iob18) {
            ctx->bindBel(uarch->get_site_bel(site_p, ctx->id("IOB18M.OUTBUF_DCIEN")), obuf_p, STRENGTH_LOCKED);
            obuf_p->attrs[id_X_IOB_SITE_TYPE] = std::string("IOB18M");
        } else {
            ctx->bindBel(uarch->get_site_bel(site_p, ctx->id("IOB33M.OUTBUF")), obuf_p, STRENGTH_LOCKED);
            obuf_p->attrs[id_X_IOB_SITE_TYPE] = std::string("IOB33M");
        }
        obuf_p->connectPort(id_DCITERMDISABLE, xil_iob->getPort(id_DCITERMDISABLE));

        CellInfo *obuf_n = insert_obuf(
                int_name(xil_iob->name, is_diff_obuf ? "N" : "OBUFTDS$subcell$N"),
                (is_diff_iobuf || (xil_iob->type == id_OBUFTDS)) ? (has_dci ? id_OBUFT_DCIEN : id_OBUFT) : id_OBUF,
                inv_i, pad_n_net, xil_iob->getPort(id_T));

        if (is_iob18) {
            ctx->bindBel(uarch->get_site_bel(site_n, ctx->id("IOB18S.OUTBUF_DCIEN")), obuf_n, STRENGTH_LOCKED);
            obuf_n->attrs[id_X_IOB_SITE_TYPE] = std::string("IOB18S");
        } else {
            ctx->bindBel(uarch->get_site_bel(site_n, ctx->id("IOB33S.OUTBUF")), obuf_n, STRENGTH_LOCKED);
            obuf_n->attrs[id_X_IOB_SITE_TYPE] = std::string("IOB33S");
        }
        obuf_n->connectPort(id_DCITERMDISABLE, xil_iob->getPort(id_DCITERMDISABLE));

        xil_iob->disconnectPort(id_DCITERMDISABLE);
    }

    // pins.cc accepts several primitives this function does not decompose.
    // Deleting the cell afterwards used to leave a bitstream with no buffer
    // and a zero exit status.  Say so instead.
    const bool iob_was_decomposed = is_se_ibuf || is_se_iobuf || is_se_obuf || is_diff_ibuf || is_diff_out_ibuf ||
                                    is_diff_iobuf || is_diff_obuf;
    const bool primitive_is_unpacked = !iob_was_decomposed;
    if (primitive_is_unpacked)
        log_error("%s '%s' has no packer. The design would otherwise build with this primitive deleted and no "
                  "buffer in the bitstream.\n",
                  xil_iob->type.c_str(ctx), ctx->nameOf(xil_iob));
}

// The fabric clock buffers an IBUFDS_GTE2 output can drive, through the
// buffer's one wire to the fabric (MGTCLKOUT).  pack_io runs before
// prepare_clocking(), so the users are still the primitives of the design:
// BUFGCE, BUFGCTRL and the BUFGMUX family have not become BUFGCTRL yet.  The
// BUFGMUX names have no constid, so the type is compared as a string, as
// prepare_clocking() does; that interns no id.
static const std::vector<std::string> ibufds_gte2_fabric_buffers{
        "BUFG", "BUFGCE", "BUFGCTRL", "BUFGMUX", "BUFGMUX_1", "BUFGMUX_CTRL", "BUFH", "BUFHCE", "BUFR"};

static bool is_ibufds_gte2_fabric_buffer(const Context *ctx, IdString type)
{
    const std::string &name = type.str(ctx);
    return std::find(ibufds_gte2_fabric_buffers.begin(), ibufds_gte2_fabric_buffers.end(), name) !=
           ibufds_gte2_fabric_buffers.end();
}

static std::string ibufds_gte2_fabric_buffer_list() { return boost::algorithm::join(ibufds_gte2_fabric_buffers, "/"); }

void XC7Packer::pack_io()
{
    log_info("Inserting IO buffers..\n");

    get_top_level_pins(ctx, toplevel_ports);
    // Insert PAD cells on top level IO, and IO buffers where one doesn't exist already
    std::vector<CellInfo *> npnr_io;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == ctx->id("$nextpnr_ibuf") || ci->type == ctx->id("$nextpnr_iobuf") ||
            ci->type == ctx->id("$nextpnr_obuf"))
            npnr_io.push_back(ci);
    }
    std::vector<std::pair<CellInfo *, PortRef>> pad_and_buf;
    for (auto ci : npnr_io) {
        pad_and_buf.push_back(insert_pad_and_buf(ci));
    }
    flush_cells();
    pool<BelId> used_io_bels;
    dict<std::string, std::string> io_bel_owner; // bel name -> IO cell name
    for (auto &iob : pad_and_buf) {
        CellInfo *pad = iob.first;
        // Process location constraints
        if (pad->attrs.count(id_PACKAGE_PIN)) {
            // Copy first: inserting LOC can reallocate the dict that the reference points into.
            const Property package_pin = pad->attrs.at(id_PACKAGE_PIN);
            pad->attrs[id_LOC] = package_pin;
        }
        if (pad->attrs.count(id_LOC)) {
            std::string loc = pad->attrs.at(id_LOC).to_string();
            BelId bel = ctx->get_package_pin_bel(ctx->id(loc));
            if (bel == BelId())
                log_error("Unable to constrain IO '%s', device does not have a pin named '%s'\n", pad->name.c_str(ctx),
                          loc.c_str());
            log_info("    Constraining '%s' to pad '%s'\n", pad->name.c_str(ctx), ctx->nameOfBel(bel));
            pad->attrs[id_BEL] = std::string(ctx->nameOfBel(bel));
        }
        // A pad whose site is already fixed (by LOC/PACKAGE_PIN) is the only
        // kind that can collide with another pad: an unconstrained one is
        // handed a free bel elsewhere.  Two ports (or one port twice) on one
        // package pin therefore collide here, and only one can actually reach
        // the pad -- say so in the names the user wrote, before the placer
        // reports it as a bel collision between $iopadmap$... cells.
        // (Port of nextpnr-xilinx 9efb656d, fixes #8.)
        bool pad_site_fixed = pad->attrs.count(id_BEL) != 0;
        if (pad_site_fixed) {
            const std::string bel_name = pad->attrs.at(id_BEL).as_string();
            auto owner = io_bel_owner.find(bel_name);
            bool site_already_claimed = owner != io_bel_owner.end();
            if (site_already_claimed) {
                const std::string loc =
                        pad->attrs.count(id_LOC) ? pad->attrs.at(id_LOC).as_string() : std::string("?");
                log_warning("Conflicting outputs: IO '%s' and IO '%s' are both constrained to package pin '%s' "
                            "(site '%s'); only one of them can drive the pad\n",
                            pad->name.c_str(ctx), owner->second.c_str(), loc.c_str(), bel_name.c_str());
            } else {
                io_bel_owner.emplace(bel_name, pad->name.str(ctx));
            }
            used_io_bels.insert(ctx->getBelByNameStr(bel_name));
        }
    }
    // A differential buffer with only one of its two ports on a package pin:
    // the other port goes on the other pin of the pair, as Vivado does (with P
    // alone, and with N alone). A pair is the master (P) and the slave (N) site
    // of one IOB tile, as check_diff_pair() below has it. Only a pad nothing
    // constrained gets here, so a design that sets both ports, or has no
    // differential buffer, is not affected.
    auto is_n_port = [&](const PortRef &buf) {
        const bool is_ibuf_n = buf.port == id_IB && buf.cell->type.in(id_IBUFDS, id_IBUFGDS, id_IBUFDS_INTERMDISABLE);
        const bool is_iobuf_n =
                buf.port == id_IOB && buf.cell->type.in(id_IOBUFDS, id_IOBUFDS_DCIEN, id_IOBUFDS_DIFF_OUT,
                                                        id_IOBUFDS_DIFF_OUT_DCIEN, id_IOBUFDS_DIFF_OUT_INTERMDISABLE);
        const bool is_obuf_n = buf.port == id_OB && buf.cell->type.in(id_OBUFDS, id_OBUFTDS);
        return is_ibuf_n || is_iobuf_n || is_obuf_n;
    };
    auto is_p_port = [&](const PortRef &buf) {
        const bool is_ibuf_p = buf.port == id_I && buf.cell->type.in(id_IBUFDS, id_IBUFGDS, id_IBUFDS_INTERMDISABLE);
        const bool is_iobuf_p =
                buf.port == id_IO && buf.cell->type.in(id_IOBUFDS, id_IOBUFDS_DCIEN, id_IOBUFDS_DIFF_OUT,
                                                       id_IOBUFDS_DIFF_OUT_DCIEN, id_IOBUFDS_DIFF_OUT_INTERMDISABLE);
        const bool is_obuf_p = buf.port == id_O && buf.cell->type.in(id_OBUFDS, id_OBUFTDS);
        return is_ibuf_p || is_iobuf_p || is_obuf_p;
    };
    auto other_port = [&](IdString port) {
        return port == id_IB    ? id_I
               : port == id_IOB ? id_IO
               : port == id_OB  ? id_O
               : port == id_I   ? id_IB
               : port == id_IO  ? id_IOB
                                : id_OB;
    };
    auto package_pin_of = [&](BelId bel) {
        const PadInfoPOD *pin = ctx->get_bel_package_pin(bel);
        return pin ? IdString(pin->package_pin).str(ctx) : std::string();
    };
    for (auto &iob : pad_and_buf) {
        CellInfo *pad = iob.first;
        const bool pad_is_placed = pad->attrs.count(id_BEL) != 0;
        const bool pad_is_n = !pad_is_placed && is_n_port(iob.second);
        const bool pad_is_p = !pad_is_placed && !pad_is_n && is_p_port(iob.second);
        const bool pad_is_half_of_a_pair = pad_is_n || pad_is_p;
        if (!pad_is_half_of_a_pair)
            continue;
        const IdString partner_port = other_port(iob.second.port);
        CellInfo *partner = nullptr;
        for (auto &other : pad_and_buf) {
            const bool is_the_partner = other.second.cell == iob.second.cell && other.second.port == partner_port;
            if (is_the_partner)
                partner = other.first;
        }
        const bool partner_is_placed = partner != nullptr && partner->attrs.count(id_BEL) != 0;
        if (!partner_is_placed)
            continue; // reported below as an unconstrained IO
        const char *side = pad_is_n ? "N" : "P";
        const char *partner_side = pad_is_n ? "P" : "N";
        const BelId partner_bel = ctx->getBelByNameStr(partner->attrs.at(id_BEL).as_string());
        const std::string partner_pin = package_pin_of(partner_bel);
        const std::string partner_bel_in_site = uarch->bel_name_in_site(partner_bel).str(ctx);
        const std::string bank = partner_bel_in_site.substr(0, 5); // IOB33 or IOB18
        const std::string partner_wants = bank + (pad_is_n ? "M.PAD" : "S.PAD");
        const bool partner_is_on_its_side_of_a_pair = partner_bel_in_site == partner_wants;
        if (!partner_is_on_its_side_of_a_pair)
            log_error("%s '%s': the %s port '%s' has no package pin, and the %s port '%s' is on %s, which is not the "
                      "%s pin of a differential pair. Give the %s port its pin, or put the %s port on an "
                      "IO_L<n>%s_... pin.\n",
                      iob.second.cell->type.c_str(ctx), ctx->nameOf(iob.second.cell), side, ctx->nameOf(pad),
                      partner_side, ctx->nameOf(partner), partner_pin.c_str(), partner_side, side, partner_side,
                      partner_side);
        const std::string wanted_bel_in_site = bank + (pad_is_n ? "S.PAD" : "M.PAD");
        BelId bel;
        const auto &tile_data = chip_tile_info(ctx->chip_info, partner_bel.tile);
        for (int32_t i = 0; i < tile_data.bels.ssize(); i++) {
            const BelId candidate(partner_bel.tile, i);
            const bool is_the_other_pad = uarch->bel_name_in_site(candidate).str(ctx) == wanted_bel_in_site;
            if (is_the_other_pad)
                bel = candidate;
        }
        const std::string pin = bel != BelId() ? package_pin_of(bel) : std::string();
        const bool package_bonds_the_pin = !pin.empty();
        if (!package_bonds_the_pin)
            log_error("%s '%s': the %s port '%s' has no package pin, and the %s site of the pair of '%s' on %s has "
                      "no pin in this package. Give the %s port its pin.\n",
                      iob.second.cell->type.c_str(ctx), ctx->nameOf(iob.second.cell), side, ctx->nameOf(pad), side,
                      ctx->nameOf(partner), partner_pin.c_str(), side);
        const bool pin_is_taken = used_io_bels.count(bel) != 0;
        if (pin_is_taken)
            log_error("%s '%s': the %s port '%s' has no package pin, and %s, the %s pin of the pair of '%s' on %s, "
                      "is already used by IO '%s'.\n",
                      iob.second.cell->type.c_str(ctx), ctx->nameOf(iob.second.cell), side, ctx->nameOf(pad),
                      pin.c_str(), side, ctx->nameOf(partner), partner_pin.c_str(),
                      io_bel_owner.at(ctx->nameOfBel(bel)).c_str());
        log_info("    Constraining '%s' to pad '%s' (%s, the %s pin of the pair of '%s' on %s)\n", ctx->nameOf(pad),
                 ctx->nameOfBel(bel), pin.c_str(), side, ctx->nameOf(partner), partner_pin.c_str());
        pad->attrs[id_PACKAGE_PIN] = pin;
        pad->attrs[id_LOC] = pin;
        pad->attrs[id_BEL] = std::string(ctx->nameOfBel(bel));
        used_io_bels.insert(bel);
        io_bel_owner.emplace(ctx->nameOfBel(bel), pad->name.str(ctx));
    }
    // The two ports of a differential buffer share one standard, and the XDC
    // may give it to either of them, as Vivado takes it: a port without one
    // takes its partner's (every pad needs one, check_valid_pad). Two
    // different standards are an error: decomposing the buffer mixed them,
    // and Vivado keeps the one set last, an order the netlist does not have.
    for (auto &iob : pad_and_buf) {
        const bool pad_is_n = is_n_port(iob.second);
        if (!pad_is_n)
            continue;
        CellInfo *pad = iob.first;
        const IdString partner_port = other_port(iob.second.port);
        CellInfo *partner = nullptr;
        for (auto &other : pad_and_buf) {
            const bool is_the_partner = other.second.cell == iob.second.cell && other.second.port == partner_port;
            if (is_the_partner)
                partner = other.first;
        }
        const bool pair_has_no_p_port = partner == nullptr;
        if (pair_has_no_p_port)
            continue;
        const bool pad_has_an_iostandard = pad->attrs.count(id_IOSTANDARD) != 0;
        const bool partner_has_an_iostandard = partner->attrs.count(id_IOSTANDARD) != 0;
        const bool only_the_partner_has_one = partner_has_an_iostandard && !pad_has_an_iostandard;
        if (only_the_partner_has_one)
            pad->attrs[id_IOSTANDARD] = partner->attrs.at(id_IOSTANDARD);
        const bool only_the_pad_has_one = pad_has_an_iostandard && !partner_has_an_iostandard;
        if (only_the_pad_has_one)
            partner->attrs[id_IOSTANDARD] = pad->attrs.at(id_IOSTANDARD);
        const bool standards_differ = pad_has_an_iostandard && partner_has_an_iostandard &&
                                      pad->attrs.at(id_IOSTANDARD) != partner->attrs.at(id_IOSTANDARD);
        if (standards_differ)
            log_error("%s '%s': its P port '%s' has IOSTANDARD %s and its N port '%s' has %s; the two ports of a "
                      "differential pair have one standard.\n",
                      iob.second.cell->type.c_str(ctx), ctx->nameOf(iob.second.cell), ctx->nameOf(partner),
                      partner->attrs.at(id_IOSTANDARD).as_string().c_str(), ctx->nameOf(pad),
                      pad->attrs.at(id_IOSTANDARD).as_string().c_str());
    }
    // Constrain unconstrained IO
    for (auto &iob : pad_and_buf) {
        CellInfo *pad = iob.first;
        if (!pad->attrs.count(id_BEL)) {
            log_error("FIXME: unconstrained IO not supported (pad %s)\n", ctx->nameOf(pad));
        }
    }
    // Decompose macro IO primitives to smaller primitives that map logically to the actual IO Bels
    for (auto &iob : pad_and_buf) {
        auto pad_cell = iob.first;
        auto buf_cell = iob.second.cell;
        if (packed_cells.count(buf_cell->name))
            continue;

        // GT clock buffers: constrain the IBUFDS_GTE2 to the GT tile's
        // dedicated buffer site, then cluster the consumers (port of
        // nextpnr-xilinx pack_io_xc7.cc)
        if (buf_cell->type == id_IBUFDS_GTE2) {
            // Both halves of the differential pair name the same buffer, so
            // this branch is reached twice.  It used to fall out the second
            // time because the first pass added the cell to packed_cells; now
            // that the cell is kept, the fact that it is already bound is what
            // says the work is done.  Without this it is constrained twice and
            // the second bindBel asserts on an occupied bel.
            if (buf_cell->bel != BelId())
                continue;
            BelId pad_bel;
            if (pad_cell->attrs.count(id_BEL))
                pad_bel = ctx->getBelByNameStr(pad_cell->attrs.at(id_BEL).as_string());
            if (pad_bel == BelId())
                log_error("IBUFDS_GTE2 pad %s is not constrained\n", pad_cell->name.c_str(ctx));
            constrain_ibufds_gt_site(buf_cell, pad_bel);
            NetInfo *net = buf_cell->getPort(id_O);
            // ODIV2 is the reference clock divided by two.  The port name is
            // taken from the cell, as in pack_clocking, so this does not intern
            // a new id.
            NetInfo *odiv2 = nullptr;
            for (auto &port : buf_cell->ports) {
                const bool port_is_odiv2 = port.first.str(ctx) == "ODIV2";
                if (port_is_odiv2)
                    odiv2 = port.second.net;
            }
            const bool no_output_connected = net == nullptr && odiv2 == nullptr;
            if (no_output_connected)
                log_error("IBUFDS_GTE2 instance %s has neither O nor ODIV2 connected\n", buf_cell->name.c_str(ctx));
            // Accepted silicon-legal consumers: GTPE2/GTXE2_COMMON (QPLL
            // clustering), GTXE2_CHANNEL direct (CPLL mode), or a fabric
            // clock buffer (MGT REFCLK straight into one of
            // ibufds_gte2_fabric_buffers).
            CellInfo *gt_common = nullptr;
            bool has_gtxe2_channel_direct = false, has_bufg_direct = false;
            const bool o_connected = net != nullptr;
            if (o_connected) {
                for (auto &usr : net->users) {
                    const bool is_gt_common = usr.cell->type.in(id_GTPE2_COMMON, id_GTXE2_COMMON);
                    const bool is_gt_channel = usr.cell->type.in(id_GTPE2_CHANNEL, id_GTXE2_CHANNEL);
                    const bool is_fabric_clock_buffer = is_ibufds_gte2_fabric_buffer(ctx, usr.cell->type);
                    if (is_gt_common)
                        gt_common = usr.cell;
                    else if (is_gt_channel)
                        has_gtxe2_channel_direct = true;
                    else if (is_fabric_clock_buffer)
                        has_bufg_direct = true;
                }
            }
            // ODIV2 has no path to a transceiver: its only pip in the GT
            // common tile goes to the buffer's MGTCLKOUT wire, which leads to
            // the fabric clock network.
            bool odiv2_drives_fabric = false;
            const bool odiv2_connected = odiv2 != nullptr;
            if (odiv2_connected) {
                for (auto &usr : odiv2->users) {
                    const bool is_fabric_clock_buffer = is_ibufds_gte2_fabric_buffer(ctx, usr.cell->type);
                    if (!is_fabric_clock_buffer)
                        log_error("IBUFDS_GTE2 instance %s output ODIV2 can only drive a %s, not %s\n",
                                  buf_cell->name.c_str(ctx), ibufds_gte2_fabric_buffer_list().c_str(),
                                  usr.cell->type.c_str(ctx));
                    odiv2_drives_fabric = true;
                }
            }
            // O reaches the fabric through that same MGTCLKOUT wire, so only
            // one of the two outputs can drive fabric clock buffers.  Vivado
            // leaves the second net unrouted (Route 35-54).
            const bool both_outputs_drive_fabric = has_bufg_direct && odiv2_drives_fabric;
            if (both_outputs_drive_fabric)
                log_error("IBUFDS_GTE2 instance %s drives fabric clock buffers from both O and ODIV2, but the "
                          "buffer has a single wire to the fabric (MGTCLKOUT); use one of the two outputs\n",
                          buf_cell->name.c_str(ctx));
            // The buffer is KEPT, not packed away.  constrain_ibufds_gt_site
            // has just bound it to a real IBUFDS_GTE2 site, and that site has
            // configuration of its own -- IN_USE, CLKCM_CFG, CLKRCV_TRST and
            // CLKSWING_CFG -- which fasm.cc emits by walking the bound cells.
            // Adding it to packed_cells deletes it before that walk, so the
            // reference-clock buffer was placed and then left switched off:
            // the transceiver got no reference clock, the PHY's MMCMs never
            // locked, and the board came up with a working CPU and a dead
            // Ethernet.  Nothing reported it, because every stage had done
            // what it was asked.
            if (gt_common) {
                constrain_gt(pad_cell, gt_common);
                continue;
            }
            if (has_gtxe2_channel_direct) {
                for (auto &usr : net->users) {
                    if (usr.cell->type.in(id_GTPE2_CHANNEL, id_GTXE2_CHANNEL))
                        constrain_gt(pad_cell, usr.cell);
                }
            }
            // With O open, ODIV2 is the output in use and was checked above.
            const bool drives_a_legal_consumer =
                    has_gtxe2_channel_direct || has_bufg_direct || (!o_connected && odiv2_drives_fabric);
            if (drives_a_legal_consumer) {
                continue;
            }
            log_error("IBUFDS_GTE2 instance %s output port must be connected to a GTPE2_COMMON, GTXE2_COMMON, "
                      "GTXE2_CHANNEL, or %s\n",
                      buf_cell->name.c_str(ctx), ibufds_gte2_fabric_buffer_list().c_str());
        }

        // This OBUF is integrated into the GTP/GTX channel pad and does not need placing
        if (buf_cell->type == id_OBUF) {
            NetInfo *net = buf_cell->getPort(id_I);
            bool driven_by_gt_channel =
                    net && net->driver.cell && net->driver.cell->type.in(id_GTPE2_CHANNEL, id_GTXE2_CHANNEL);
            if (driven_by_gt_channel) {
                packed_cells.insert(buf_cell->name);
                constrain_gt(pad_cell, net->driver.cell);
                continue;
            }
        }
        // This IBUF is integrated into the GTP/GTX channel pad and does not need placing
        if (buf_cell->type == id_IBUF) {
            NetInfo *net = buf_cell->getPort(id_O);
            bool feeds_only_a_gt_channel = net && net->users.entries() == 1 &&
                                           (*net->users.begin()).cell->type.in(id_GTPE2_CHANNEL, id_GTXE2_CHANNEL);
            if (feeds_only_a_gt_channel) {
                packed_cells.insert(buf_cell->name);
                constrain_gt(pad_cell, (*net->users.begin()).cell);
                continue;
            }
        }

        decompose_iob(buf_cell, true, str_or_default(iob.first->attrs, id_IOSTANDARD, ""));
        packed_cells.insert(buf_cell->name);
    }
    flush_cells();

    dict<IdString, XFormRule> hriobuf_rules, hpiobuf_rules;
    hriobuf_rules[id_OBUF].new_type = id_IOB33_OUTBUF;
    hriobuf_rules[id_OBUF].port_xform[id_I] = id_IN;
    hriobuf_rules[id_OBUF].port_xform[id_O] = id_OUT;
    hriobuf_rules[id_OBUF].port_xform[id_T] = id_TRI;
    hriobuf_rules[id_OBUFT] = XFormRule(hriobuf_rules[id_OBUF]);

    hriobuf_rules[id_IBUF].new_type = id_IOB33_INBUF_EN;
    hriobuf_rules[id_IBUF].port_xform[id_I] = id_PAD;
    hriobuf_rules[id_IBUF].port_xform[id_O] = id_OUT;
    hriobuf_rules[id_IBUF_INTERMDISABLE] = XFormRule(hriobuf_rules[id_IBUF]);
    hriobuf_rules[id_IBUF_IBUFDISABLE] = XFormRule(hriobuf_rules[id_IBUF]);
    hriobuf_rules[id_IBUFDS_INTERMDISABLE_INT] = XFormRule(hriobuf_rules[id_IBUF]);
    hriobuf_rules[id_IBUFDS_INTERMDISABLE_INT].port_xform[id_IB] = id_DIFFI_IN;
    hriobuf_rules[id_IBUFDS] = XFormRule(hriobuf_rules[id_IBUF]);
    hriobuf_rules[id_IBUFDS].port_xform[id_IB] = id_DIFFI_IN;
    // IBUFGDS: legacy clock-capable spelling of IBUFDS; on 7-series they are
    // the same primitive (UG953 documents IBUFGDS only as a Spartan-6-and-
    // earlier name, and Vivado still accepts it), so treat it as an alias.
    hriobuf_rules[id_IBUFGDS] = XFormRule(hriobuf_rules[id_IBUF]);
    hriobuf_rules[id_IBUFGDS].port_xform[id_IB] = id_DIFFI_IN;

    hpiobuf_rules[id_OBUF].new_type = id_IOB18_OUTBUF_DCIEN;
    hpiobuf_rules[id_OBUF].port_xform[id_I] = id_IN;
    hpiobuf_rules[id_OBUF].port_xform[id_O] = id_OUT;
    hpiobuf_rules[id_OBUF].port_xform[id_T] = id_TRI;
    hpiobuf_rules[id_OBUFT] = XFormRule(hpiobuf_rules[id_OBUF]);

    hpiobuf_rules[id_IBUF].new_type = id_IOB18_INBUF_DCIEN;
    hpiobuf_rules[id_IBUF].port_xform[id_I] = id_PAD;
    hpiobuf_rules[id_IBUF].port_xform[id_O] = id_OUT;
    hpiobuf_rules[id_IBUF_INTERMDISABLE] = XFormRule(hpiobuf_rules[id_IBUF]);
    hpiobuf_rules[id_IBUF_IBUFDISABLE] = XFormRule(hpiobuf_rules[id_IBUF]);
    hriobuf_rules[id_IBUFDS_INTERMDISABLE_INT] = XFormRule(hriobuf_rules[id_IBUF]);
    hpiobuf_rules[id_IBUFDS_INTERMDISABLE_INT].port_xform[id_IB] = id_DIFFI_IN;
    hpiobuf_rules[id_IBUFDS] = XFormRule(hpiobuf_rules[id_IBUF]);
    hpiobuf_rules[id_IBUFDS].port_xform[id_IB] = id_DIFFI_IN;
    hpiobuf_rules[id_IBUFGDS] = XFormRule(hpiobuf_rules[id_IBUF]);
    hpiobuf_rules[id_IBUFGDS].port_xform[id_IB] = id_DIFFI_IN;

    // Special xform for OBUFx and IBUFx.
    dict<IdString, XFormRule> rules;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (!ci->attrs.count(id_BEL) && ci->bel == BelId())
            continue;
        bool is_io_buffer_cell = hriobuf_rules.count(ci->type) || hpiobuf_rules.count(ci->type);
        if (!is_io_buffer_cell)
            continue; // e.g. a GT channel bel
        std::string belname =
                ci->attrs.count(id_BEL) ? ci->attrs[id_BEL].as_string() : std::string(ctx->nameOfBel(ci->bel));
        size_t pos = belname.find(".");
        if (belname.substr(pos + 1, 5) == "IOB18")
            rules = hpiobuf_rules;
        else if (belname.substr(pos + 1, 5) == "IOB33")
            rules = hriobuf_rules;
        else if (belname.substr(pos + 1, 3) == "PAD")
            continue; // GT pads (OPAD/IPAD): no IO-buffer decomposition (handled by GT clustering)
        else
            log_error("Unexpected IOBUF BEL %s\n", belname.c_str());
        if (rules.count(ci->type)) {
            xform_cell(rules, ci);
        }
    }

    dict<IdString, XFormRule> hrio_rules;
    hrio_rules[id_PAD].new_type = id_PAD;

    hrio_rules[id_INV].new_type = id_INVERTER;
    hrio_rules[id_INV].port_xform[id_I] = id_IN;
    hrio_rules[id_INV].port_xform[id_O] = id_OUT;

    hrio_rules[id_PS7].new_type = id_PS7_PS7;
    hrio_rules[id_PCIE_2_1].new_type = id_PCIE_2_1_PCIE_2_1;

    generic_xform(hrio_rules, true);

    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        std::string type = ci->type.str(ctx);
        if (!boost::starts_with(type, "IOB33") && !boost::starts_with(type, "IOB18"))
            continue;
        if (!ci->attrs.count(id_X_IOB_SITE_TYPE))
            continue;
        type.replace(0, 5, ci->attrs.at(id_X_IOB_SITE_TYPE).as_string());
        ci->type = ctx->id(type);
    }

    // check all PAD cells for IOSTANDARD/DRIVE
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        std::string type = ci->type.str(ctx);
        if (type != "PAD")
            continue;
        check_valid_pad(ci, type);
    }
}

void XC7Packer::check_valid_pad(CellInfo *ci, std::string type)
{
    // GT pads (OPAD/IPAD sites) don't need IOSTANDARD constraints
    const std::string belname = ci->bel != BelId() ? ctx->nameOfBel(ci->bel) : str_or_default(ci->attrs, id_BEL);
    {
        bool is_gt_pad = boost::contains(belname, "OPAD") || boost::contains(belname, "IPAD");
        if (is_gt_pad)
            return;
    }
    auto iostandard_id = id_IOSTANDARD;
    auto iostandard_attr = ci->attrs.find(iostandard_id);
    if (iostandard_attr == ci->attrs.end())
        log_error("port %s has no IOSTANDARD property\n", ci->name.c_str(ctx));

    auto iostandard = iostandard_attr->second.as_string();
    if (!boost::starts_with(iostandard, "LVTTL") && !boost::starts_with(iostandard, "LVCMOS"))
        return;

    // no drive strength attribute: use default
    if (!ci->attrs.count(id_DRIVE))
        return;
    auto drive = int_or_default(ci->attrs, id_DRIVE, 0);

    // The cell type is "PAD" here, so the bank type comes from the site the pad
    // sits on.  Testing the type never matched and sent every pad down the
    // high-performance branch below.
    bool is_iob33 = boost::contains(belname, "IOB33");
    if (is_iob33) {
        if (drive == 4 || drive == 8 || drive == 12)
            return;
        if (iostandard != "LVCMOS12" && drive == 16)
            return;
        if ((iostandard == "LVCMOS18" || iostandard == "LVTTL") && drive == 24)
            return;
    } else { // IOB18
        if (drive == 2 || drive == 4 || drive == 6 || drive == 8)
            return;
        if (iostandard != "LVCMOS12" && (drive == 12 || drive == 16))
            return;
    }

    log_error("unsupported DRIVE strength %d for port %s\n", drive, ci->name.c_str(ctx));
}

SiteIndex XC7Packer::get_ologic_site(BelId io_bel)
{
    BelId ibc_bel;
    if (boost::contains(uarch->bel_name_in_site(io_bel).str(ctx), "IOB18"))
        ibc_bel = uarch->get_site_bel(uarch->get_bel_site(io_bel), ctx->id("IOB18.OUTBUF_DCIEN"));
    else
        ibc_bel = uarch->get_site_bel(uarch->get_bel_site(io_bel), ctx->id("IOB33.OUTBUF"));
    NPNR_ASSERT(ibc_bel != BelId());

    std::queue<WireId> visit;
    visit.push(ctx->getBelPinWire(ibc_bel, id_IN));

    while (!visit.empty()) {
        WireId cursor = visit.front();
        visit.pop();
        for (auto bp : ctx->getWireBelPins(cursor)) {
            auto site = uarch->get_bel_site(bp.bel);
            if (boost::starts_with(uarch->get_site_name(site).str(ctx), "OLOGIC"))
                return site;
        }
        for (auto pip : ctx->getPipsUphill(cursor))
            visit.push(ctx->getPipSrcWire(pip));
    }
    NPNR_ASSERT_FALSE("failed to find OLOGIC");
}

SiteIndex XC7Packer::get_ilogic_site(BelId io_bel)
{
    BelId ibc_bel;
    if (boost::contains(uarch->bel_name_in_site(io_bel).str(ctx), "IOB18"))
        ibc_bel = uarch->get_site_bel(uarch->get_bel_site(io_bel), ctx->id("IOB18.INBUF_DCIEN"));
    else
        ibc_bel = uarch->get_site_bel(uarch->get_bel_site(io_bel), ctx->id("IOB33.INBUF_EN"));

    NPNR_ASSERT(ibc_bel != BelId());

    std::queue<WireId> visit;
    visit.push(ctx->getBelPinWire(ibc_bel, id_OUT));

    while (!visit.empty()) {
        WireId cursor = visit.front();
        visit.pop();
        for (auto bp : ctx->getWireBelPins(cursor)) {
            auto site = uarch->get_bel_site(bp.bel);
            if (boost::starts_with(uarch->get_site_name(site).str(ctx), "ILOGIC"))
                return site;
        }
        for (auto pip : ctx->getPipsDownhill(cursor))
            visit.push(ctx->getPipDstWire(pip));
    }
    NPNR_ASSERT_FALSE("failed to find ILOGIC");
}

SiteIndex XC7Packer::get_idelay_site(BelId io_bel)
{
    BelId ibc_bel;
    if (boost::contains(uarch->bel_name_in_site(io_bel).str(ctx), "IOB18"))
        ibc_bel = uarch->get_site_bel(uarch->get_bel_site(io_bel), ctx->id("IOB18.INBUF_DCIEN"));
    else
        ibc_bel = uarch->get_site_bel(uarch->get_bel_site(io_bel), ctx->id("IOB33.INBUF_EN"));

    NPNR_ASSERT(ibc_bel != BelId());

    std::queue<WireId> visit;
    visit.push(ctx->getBelPinWire(ibc_bel, id_OUT));

    while (!visit.empty()) {
        WireId cursor = visit.front();
        visit.pop();
        for (auto bp : ctx->getWireBelPins(cursor)) {
            auto site = uarch->get_bel_site(bp.bel);
            if (boost::starts_with(uarch->get_site_name(site).str(ctx), "IDELAY"))
                return site;
        }
        for (auto pip : ctx->getPipsDownhill(cursor))
            visit.push(ctx->getPipDstWire(pip));
    }
    NPNR_ASSERT_FALSE("failed to find IDELAY");
}

SiteIndex XC7Packer::get_odelay_site(BelId io_bel)
{
    BelId obc_bel;
    if (boost::contains(uarch->bel_name_in_site(io_bel).str(ctx), "IOB18"))
        obc_bel = uarch->get_site_bel(uarch->get_bel_site(io_bel), ctx->id("IOB18.OUTBUF_DCIEN"));
    else
        log_error("BEL %s is located on a high range bank. High range banks do not have ODELAY\n",
                  ctx->nameOfBel(io_bel));

    std::queue<WireId> visit;
    visit.push(ctx->getBelPinWire(obc_bel, id_IN));

    while (!visit.empty()) {
        WireId cursor = visit.front();
        visit.pop();
        for (auto bp : ctx->getWireBelPins(cursor)) {
            auto site = uarch->get_bel_site(bp.bel);
            if (boost::starts_with(uarch->get_site_name(site).str(ctx), "ODELAY"))
                return site;
        }
        for (auto pip : ctx->getPipsUphill(cursor))
            visit.push(ctx->getPipSrcWire(pip));
    }
    NPNR_ASSERT_FALSE("failed to find ODELAY");
}

SiteIndex XC7Packer::get_ioctrl_site(BelId io_bel)
{
    int hclk_tile = uarch->hclk_for_iob(io_bel);
    const auto &extra_data = uarch->tile_extra_data(hclk_tile);

    for (int site = 0; site < extra_data->sites.ssize(); site++) {
        auto &site_data = extra_data->sites[site];
        if (boost::starts_with(IdString(site_data.name_prefix).str(ctx), "IDELAYCTRL"))
            return SiteIndex(hclk_tile, site);
    }

    NPNR_ASSERT_FALSE("failed to find IOCTRL");
}

void XC7Packer::fold_inverter(CellInfo *cell, std::string port)
{
    IdString p = ctx->id(port);
    NetInfo *net = cell->getPort(p);
    if (!net)
        return;
    CellInfo *drv = net->driver.cell;
    if (!drv)
        return;
    if (drv->type == id_LUT1 && int_or_default(drv->params, id_INIT, 0) == 1) {
        cell->disconnectPort(p);
        NetInfo *preinv = drv->getPort(id_I0);
        cell->connectPort(p, preinv);
        cell->params[ctx->idf("IS_%s_INVERTED", port.c_str())] = 1;
        if (net->users.empty())
            packed_cells.insert(drv->name);
    } else if (drv->type == id_INV) {
        cell->disconnectPort(p);
        NetInfo *preinv = drv->getPort(id_I);
        cell->connectPort(p, preinv);
        cell->params[ctx->idf("IS_%s_INVERTED", port.c_str())] = 1;
        if (net->users.empty())
            packed_cells.insert(drv->name);
    }
}

void XC7Packer::pack_iologic()
{
    log_info("Packing IOLOGIC...\n");
    dict<IdString, BelId> iodelay_to_io;
    dict<IdString, XFormRule> iologic_rules;

    // IDDR
    iologic_rules[id_IDDR].new_type = id_ILOGICE3_IFF;
    iologic_rules[id_IDDR].port_multixform[id_C] = {id_CK, id_CKB};
    iologic_rules[id_IDDR].port_xform[id_S] = id_SR;
    iologic_rules[id_IDDR].port_xform[id_R] = id_SR;

    // SERDES
    iologic_rules[id_ISERDESE2].new_type = id_ISERDESE2_ISERDESE2;
    iologic_rules[id_OSERDESE2].new_type = id_OSERDESE2_OSERDESE2;

    // DELAY
    iologic_rules[id_IDELAYE2].new_type = id_IDELAYE2_IDELAYE2;
    iologic_rules[id_ODELAYE2].new_type = id_ODELAYE2_ODELAYE2;

    // Handles pseudo-diff output buffers without finding multiple sinks
    auto find_p_outbuf = [&](NetInfo *net) {
        CellInfo *outbuf = nullptr;
        for (auto &usr : net->users) {
            IdString type = usr.cell->type;
            if (type.in(id_IOB33_OUTBUF, id_IOB33M_OUTBUF, id_IOB18_OUTBUF_DCIEN, id_IOB18M_OUTBUF_DCIEN)) {
                if (outbuf != nullptr)
                    return (CellInfo *)nullptr; // drives multiple outputs
                outbuf = usr.cell;
            } else if (type == id_ODELAYE2) {
                auto dataout = usr.cell->ports.find(id_DATAOUT);
                if (dataout != usr.cell->ports.end()) {
                    for (auto &user : dataout->second.net->users) {
                        IdString dataout_type = user.cell->type;
                        if (dataout_type.in(id_IOB18_OUTBUF_DCIEN, id_IOB18M_OUTBUF_DCIEN)) {
                            if (outbuf != nullptr)
                                return (CellInfo *)nullptr; // drives multiple outputs
                            outbuf = user.cell;
                        }
                    }
                } else {
                    if (outbuf != nullptr)
                        return (CellInfo *)nullptr; // drives multiple outputs
                }
            }
        }
        return outbuf;
    };

    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_IDELAYE2) {
            NetInfo *d = ci->getPort(id_IDATAIN);
            if (!d || !d->driver.cell)
                log_error("%s '%s' has disconnected IDATAIN input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            CellInfo *drv = d->driver.cell;
            BelId io_bel;
            if (boost::contains(drv->type.str(ctx), "INBUF_EN") || boost::contains(drv->type.str(ctx), "INBUF_DCIEN"))
                io_bel = drv->bel;
            else
                log_error("%s '%s' has IDATAIN input connected to illegal cell type %s\n", ci->type.c_str(ctx),
                          ctx->nameOf(ci), drv->type.c_str(ctx));
            SiteIndex iol_site = get_idelay_site(io_bel);

            BelId idelay_bel = uarch->get_site_bel(iol_site, id_IDELAYE2);
            NPNR_ASSERT(idelay_bel != BelId());
            log_info("   binding input delay cell '%s' to bel '%s'\n", ctx->nameOf(ci), ctx->nameOfBel(idelay_bel));
            ctx->bindBel(idelay_bel, ci, STRENGTH_LOCKED);

            ci->attrs[id_X_IO_BEL] = ctx->getBelName(io_bel).str(ctx);
            iodelay_to_io[ci->name] = io_bel;
        } else if (ci->type == id_ODELAYE2) {
            NetInfo *dataout = ci->getPort(id_DATAOUT);
            if (!dataout || dataout->users.empty())
                log_error("%s '%s' has disconnected DATAOUT input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            BelId io_bel;
            auto no_users = dataout->users.entries();
            for (auto userport : dataout->users) {
                CellInfo *user = userport.cell;
                auto user_type = user->type.str(ctx);
                // OBUFDS has the negative pin connected to an inverter
                if (no_users == 2 && user_type == "INVERTER")
                    continue;
                // yosys emits a plain IOB33_OUTBUF (no tristate/DCI suffix) when
                // an output's tristate is constant-enabled, and ODELAYE2's
                // DATAOUT legitimately drives that shape as well as the
                // OUTBUF_EN/OUTBUF_DCIEN ones.
                if (boost::contains(user_type, "OUTBUF"))
                    io_bel = user->bel;
                else
                    // TODO: support SIGNAL_PATTERN = CLOCK
                    log_error("%s '%s' has DATAOUT connected to unsupported cell type %s\n", ci->type.c_str(ctx),
                              ctx->nameOf(ci), user_type.c_str());
            }
            SiteIndex iol_site = get_odelay_site(io_bel);

            BelId odelay_bel = uarch->get_site_bel(iol_site, id_ODELAYE2);
            NPNR_ASSERT(odelay_bel != BelId());
            log_info("   binding output delay cell '%s' to bel '%s'\n", ctx->nameOf(ci), ctx->nameOfBel(odelay_bel));
            ctx->bindBel(odelay_bel, ci, STRENGTH_LOCKED);

            ci->attrs[id_X_IO_BEL] = ctx->getBelName(io_bel).str(ctx);
            iodelay_to_io[ci->name] = io_bel;
        }
    }

    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_ODDR) {
            NetInfo *q = ci->getPort(id_Q);
            if (q == nullptr || q->users.empty())
                log_error("%s '%s' has disconnected Q output\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            BelId io_bel;
            CellInfo *ob = find_p_outbuf(q);
            if (ob != nullptr)
                io_bel = ob->bel;
            else
                log_error("%s '%s' has illegal fanout on Q output\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            SiteIndex ol_site = get_ologic_site(io_bel);

            PortRef dest_port = *q->users.begin();
            auto is_tristate = dest_port.port == id_TRI;

            dict<IdString, XFormRule> oddr_rules;
            if (boost::contains(uarch->get_site_name(ol_site).str(ctx), "IOB18"))
                oddr_rules[id_ODDR].new_type = is_tristate ? id_OLOGICE2_TFF : id_OLOGICE2_OUTFF;
            else
                oddr_rules[id_ODDR].new_type = is_tristate ? id_OLOGICE3_TFF : id_OLOGICE3_OUTFF;
            oddr_rules[id_ODDR].port_xform[id_C] = id_CK;
            oddr_rules[id_ODDR].port_xform[id_S] = id_SR;
            oddr_rules[id_ODDR].port_xform[id_R] = id_SR;
            xform_cell(oddr_rules, ci);

            BelId oddr_bel = uarch->get_site_bel(ol_site, is_tristate ? ctx->id("TFF") : ctx->id("OUTFF"));
            NPNR_ASSERT(oddr_bel != BelId());
            log_info("   binding output DDR cell '%s' to bel '%s'\n", ctx->nameOf(ci), ctx->nameOfBel(oddr_bel));
            ctx->bindBel(oddr_bel, ci, STRENGTH_LOCKED);
        } else if (ci->type == id_OSERDESE2) {

            NetInfo *shiftin1 = ci->getPort(id_SHIFTIN1), *shiftin2 = ci->getPort(id_SHIFTIN2),
                    *tbytein = ci->getPort(id_TBYTEIN);

            // If connected to ground (i.e. unused) this can't actually be routed so remove it
            if (shiftin1 && shiftin1->name == ctx->id("$PACKER_GND_NET")) {
                ci->disconnectPort(id_SHIFTIN1);
                shiftin1 = nullptr;
            }
            if (shiftin2 && shiftin2->name == ctx->id("$PACKER_GND_NET")) {
                ci->disconnectPort(id_SHIFTIN2);
                shiftin2 = nullptr;
            }
            if (tbytein && tbytein->name == ctx->id("$PACKER_GND_NET")) {
                ci->disconnectPort(id_TBYTEIN);
                tbytein = nullptr;
            }

            auto serdes_mode = str_or_default(ci->params, id_SERDES_MODE, "MASTER");

            if (serdes_mode == "MASTER") {
                NetInfo *q = ci->getPort(id_OQ);
                NetInfo *ofb = ci->getPort(id_OFB);
                bool q_disconnected = !q || q->users.empty();
                bool ofb_disconnected = !ofb || ofb->users.empty();
                if (q_disconnected && ofb_disconnected) {
                    log_error("%s '%s' has disconnected OQ/OFB output ports\n", ci->type.c_str(ctx), ctx->nameOf(ci));
                }
                BelId io_bel;
                CellInfo *ob = !q_disconnected ? find_p_outbuf(q) : find_p_outbuf(ofb);
                bool ofb_loops_back_to_iserdes =
                        ofb && ofb->users.entries() == 1 && (*ofb->users.begin()).cell->type == id_ISERDESE2;
                if (ob != nullptr) {
                    io_bel = ob->bel;
                } else if (ofb_loops_back_to_iserdes) {
                    // OFB loopback (e.g. DDR read calibration): OSERDESE2's OFB
                    // feeds an ISERDESE2, so there is no IOB to anchor on; the
                    // pair is bound to a free OSERDES/ISERDES site pair after
                    // the main loop.  (Port of nextpnr-xilinx OFB support.)
                    unconstrained_oserdes.insert(ci);
                    continue;
                } else {
                    log_error("%s '%s' has illegal fanout on OQ or OFB output\n", ci->type.c_str(ctx), ctx->nameOf(ci));
                }

                SiteIndex ol_site = get_ologic_site(io_bel);

                BelId oserdes_bel = uarch->get_site_bel(ol_site, id_OSERDESE2);
                NPNR_ASSERT(oserdes_bel != BelId());
                log_info("   binding output SERDES cell '%s' to bel '%s'\n", ctx->nameOf(ci),
                         ctx->nameOfBel(oserdes_bel));
                ctx->bindBel(oserdes_bel, ci, STRENGTH_LOCKED);

                if (shiftin1 != nullptr || shiftin2 != nullptr) {
                    CellInfo *cascaded_cell = shiftin1 ? shiftin1->driver.cell : shiftin2->driver.cell;
                    if (!cascaded_cell || !cascaded_cell->type.in(id_OSERDESE2))
                        log_error("OSERDESE2 cell '%s' has SHIFTIN driven by illegal cell '%s'\n", ctx->nameOf(ci),
                                  cascaded_cell ? ctx->nameOf(cascaded_cell) : "");

                    // cascaded location is one below master, so place the cascaded cell there
                    SiteIndex cascade_site = uarch->rel_site(ol_site, 0, -1);
                    NPNR_ASSERT(cascade_site != SiteIndex());
                    BelId cascade_bel = uarch->get_site_bel(cascade_site, id_OSERDESE2);
                    NPNR_ASSERT(cascade_bel != BelId());
                    log_info("   binding cascaded output SERDES cell '%s' to bel '%s'\n", ctx->nameOf(cascaded_cell),
                             ctx->nameOfBel(cascade_bel));
                    ctx->bindBel(cascade_bel, cascaded_cell, STRENGTH_LOCKED);
                }

            } else if (serdes_mode == "SLAVE") {

                NetInfo *shiftout1 = ci->getPort(id_SHIFTOUT1);
                NetInfo *shiftout2 = ci->getPort(id_SHIFTOUT2);
                if (!shiftout1 && !shiftout2)
                    log_error("OSERDESE2 cell '%s' with SERDES_MODE SLAVE must have SHIFTOUT1/2 connected.\n",
                              ctx->nameOf(ci));
                for (auto net : {shiftout1, shiftout2}) {
                    if (!net)
                        continue;
                    if (net->users.entries() != 1)
                        log_error(
                                "OSERDESE2 cell '%s' with SERDES_MODE SLAVE has multiple fanout on SHIFTOUT net '%s'\n",
                                ctx->nameOf(ci), ctx->nameOf(net));
                    auto usr = *(net->users.begin());
                    if (usr.cell->type != id_OSERDESE2 || !usr.port.in(id_SHIFTIN1, id_SHIFTIN2))
                        log_error("OSERDESE2 cell '%s' has SHIFTOUT driving illegal cell port '%s.%s'\n",
                                  ctx->nameOf(ci), ctx->nameOf(usr.cell), ctx->nameOf(usr.port));
                }
            } else {
                log_error("OSERDESE2 cell '%s' has unsupported SERDES_MODE '%s'\n", ctx->nameOf(ci),
                          serdes_mode.c_str());
            }

        } else if (ci->type == id_IDDR) {
            fold_inverter(ci, "C");

            BelId io_bel;
            NetInfo *d = ci->getPort(id_D);
            if (!d || !d->driver.cell)
                log_error("%s '%s' has disconnected D input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
            CellInfo *drv = d->driver.cell;
            if (boost::contains(drv->type.str(ctx), "INBUF_EN") || boost::contains(drv->type.str(ctx), "INBUF_DCIEN"))
                io_bel = drv->bel;
            else if (boost::contains(drv->type.str(ctx), "IDELAYE2") && d->driver.port == id_DATAOUT)
                io_bel = iodelay_to_io.at(drv->name);
            else
                log_error("%s '%s' has D input connected to illegal cell type %s\n", ci->type.c_str(ctx),
                          ctx->nameOf(ci), drv->type.c_str(ctx));

            SiteIndex il_site = get_ilogic_site(io_bel);

            BelId iddr_bel = uarch->get_site_bel(il_site, ctx->id("IFF"));
            NPNR_ASSERT(iddr_bel != BelId());
            log_info("   binding input DDR cell '%s' to bel '%s'\n", ctx->nameOf(ci), ctx->nameOfBel(iddr_bel));
            ctx->bindBel(iddr_bel, ci, STRENGTH_LOCKED);
        } else if (ci->type == id_ISERDESE2) {
            fold_inverter(ci, "CLKB");
            fold_inverter(ci, "OCLKB");

            // OFB loopback: the data path is the partner OSERDESE2's OFB
            // output, so the cell has no IOB / IDELAYE2 to anchor on and is
            // bound next to its OSERDESE2 in the unconstrained pass below.
            // Two shapes exist: the OSERDESE2's OFB drives the ISERDESE2's D,
            // or it drives the ISERDESE2's OFB input while D and DDLY are
            // deliberately left open -- the DDR3 read-calibration training
            // ISERDES, exactly as Xilinx's DDR3 PHY example (and Vivado)
            // instantiates it.
            {
                NetInfo *d_ofb = ci->getPort(id_D);
                const bool d_driven_by_oserdes_ofb = d_ofb && d_ofb->driver.cell &&
                                                     d_ofb->driver.cell->type == id_OSERDESE2 &&
                                                     d_ofb->driver.port == id_OFB;
                NetInfo *ofb_in = ci->getPort(id_OFB);
                const bool ofb_input_driven_by_oserdes_ofb =
                        ofb_in && ofb_in->driver.cell && ofb_in->driver.cell->type == id_OSERDESE2 &&
                        ofb_in->driver.port == id_OFB;
                const bool data_comes_from_oserdes_ofb = d_driven_by_oserdes_ofb || ofb_input_driven_by_oserdes_ofb;
                if (data_comes_from_oserdes_ofb)
                    continue;
            }

            std::string iobdelay = str_or_default(ci->params, id_IOBDELAY, "NONE");
            BelId io_bel;

            if (iobdelay == "IFD") {
                NetInfo *d = ci->getPort(id_DDLY);
                if (!d || !d->driver.cell)
                    log_error("%s '%s' has disconnected DDLY input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
                CellInfo *drv = d->driver.cell;
                if (boost::contains(drv->type.str(ctx), "IDELAYE2") && d->driver.port == id_DATAOUT)
                    io_bel = iodelay_to_io.at(drv->name);
                else
                    log_error("%s '%s' has DDLY input connected to illegal cell type %s\n", ci->type.c_str(ctx),
                              ctx->nameOf(ci), drv->type.c_str(ctx));
            } else if (iobdelay == "NONE") {
                NetInfo *d = ci->getPort(id_D);
                if (!d || !d->driver.cell)
                    log_error("%s '%s' has disconnected D input\n", ci->type.c_str(ctx), ctx->nameOf(ci));
                CellInfo *drv = d->driver.cell;
                if (boost::contains(drv->type.str(ctx), "INBUF_EN") ||
                    boost::contains(drv->type.str(ctx), "INBUF_DCIEN"))
                    io_bel = drv->bel;
                else
                    log_error("%s '%s' has D input connected to illegal cell type %s\n", ci->type.c_str(ctx),
                              ctx->nameOf(ci), drv->type.c_str(ctx));
            } else {
                log_error("%s '%s' has unsupported IOBDELAY value '%s'\n", ci->type.c_str(ctx), ctx->nameOf(ci),
                          iobdelay.c_str());
            }

            SiteIndex il_site = get_ilogic_site(io_bel);

            BelId iserdes_bel = uarch->get_site_bel(il_site, id_ISERDESE2);
            NPNR_ASSERT(iserdes_bel != BelId());
            log_info("   binding input SERDES cell '%s' to bel '%s'\n", ctx->nameOf(ci), ctx->nameOfBel(iserdes_bel));
            ctx->bindBel(iserdes_bel, ci, STRENGTH_LOCKED);
        }
    }

    // OFB-loopback pairs: an OSERDESE2 whose OFB feeds an ISERDESE2 has no
    // IOB to anchor on, so bind the pair to a free OSERDES/ISERDES site pair
    // -- one whose tile can actually carry the feedback, and the bel the
    // design pinned if it pinned one.
    // The two cells only work as a pair -- the feedback runs inside one tile --
    // so a pin on either decides where both go, and a pin that would separate
    // them is reported instead of honoured.
    // (Port of nextpnr-xilinx unconstrained-OSERDESE2 support.)
    //
    // A pinned bel is resolved by name and checked to be of the type the cell
    // needs: a name that resolves to another site type would otherwise be bound
    // to a cell that cannot sit there.
    auto pinned_bel = [&](CellInfo *cell, IdString expected_type) {
        const std::string name = cell->attrs.at(id_BEL).as_string();
        BelId bel = BelId();
        try {
            bel = ctx->getBelByNameStr(name);
        } catch (const std::out_of_range &) {
            // a name whose tile is not on this device throws instead of
            // returning an empty bel
        }
        const bool bel_exists = (bel != BelId());
        if (!bel_exists)
            log_error("%s '%s' is pinned to a bel this device does not have ('%s')\n", cell->type.c_str(ctx),
                      ctx->nameOf(cell), name.c_str());
        const bool bel_is_the_type_the_cell_needs = ctx->getBelType(bel) == expected_type;
        if (!bel_is_the_type_the_cell_needs)
            log_error("%s '%s' is pinned to bel '%s', which is a %s, not a %s\n", cell->type.c_str(ctx),
                      ctx->nameOf(cell), name.c_str(), ctx->getBelType(bel).c_str(ctx), expected_type.c_str(ctx));
        return bel;
    };

    for (auto ci : unconstrained_oserdes) {
        NetInfo *ofb = ci->getPort(id_OFB);
        NPNR_ASSERT(ofb != nullptr && ofb->users.entries() == 1);
        CellInfo *iserdes = (*ofb->users.begin()).cell;
        NPNR_ASSERT(iserdes->type == id_ISERDESE2);

        const bool oserdes_was_pinned = ci->attrs.count(id_BEL) != 0;
        const bool iserdes_was_pinned = iserdes->attrs.count(id_BEL) != 0;

        BelId oserdes_bel = BelId();
        if (oserdes_was_pinned) {
            oserdes_bel = pinned_bel(ci, id_OSERDESE2_OSERDESE2);
        } else if (iserdes_was_pinned) {
            // The OSERDESE2 has to land in the OLOGIC half of whatever tile the
            // ISERDESE2 was pinned into.
            BelId pinned_iserdes_bel = pinned_bel(iserdes, id_ISERDESE2_ISERDESE2);
            SiteIndex ol_site = get_ologic_site_for_ilogic(uarch->get_bel_site(pinned_iserdes_bel));
            oserdes_bel = uarch->get_site_bel(ol_site, id_OSERDESE2);
        } else {
            for (auto bel : ctx->getBels()) {
                bool is_free_oserdes_bel = ctx->getBelType(bel) == id_OSERDESE2_OSERDESE2 && ctx->checkBelAvail(bel);
                if (!is_free_oserdes_bel)
                    continue;
                // A _SING I/O tile carries a single IOLOGIC/OLOGIC pair and the
                // database has no feedback pips for it, so a pair placed there
                // captures nothing: the DDR3 PHY's read-training ISERDES then
                // sits in BITSLIP_DQS_TRAIN_1 for ever and the controller never
                // reaches DONE_CALIBRATE.  Measured on an Arty S7 (xc7s50): the
                // pair the placer put in LIOI3_SING_X0Y149 never saw the
                // training pattern, the one in LIOI3_X0Y147 did.
                const bool tile_has_no_feedback_pips = boost::contains(uarch->tile_name(bel.tile), "_SING");
                if (tile_has_no_feedback_pips)
                    continue;
                oserdes_bel = bel;
                break;
            }
        }
        if (oserdes_bel == BelId())
            log_error("IO placer ran out of available OSERDESE2 bels (%d unconstrained)\n",
                      int(unconstrained_oserdes.size()));
        const bool oserdes_bel_is_taken = !ctx->checkBelAvail(oserdes_bel);
        if (oserdes_bel_is_taken)
            log_error("OSERDESE2 '%s' cannot take bel '%s': '%s' holds it\n", ctx->nameOf(ci), ctx->nameOfBel(oserdes_bel),
                      ctx->nameOf(ctx->getBoundBelCell(oserdes_bel)));
        ctx->bindBel(oserdes_bel, ci, STRENGTH_LOCKED);

        BelId iserdes_bel = BelId();
        if (iserdes_was_pinned) {
            iserdes_bel = pinned_bel(iserdes, id_ISERDESE2_ISERDESE2);
        } else {
            SiteIndex il_site = get_ilogic_site_for_ologic(uarch->get_bel_site(oserdes_bel));
            iserdes_bel = uarch->get_site_bel(il_site, id_ISERDESE2);
        }
        NPNR_ASSERT(iserdes_bel != BelId());
        // The feedback the pair exists for runs between the two sites of one
        // tile, so a pin that puts them in different tiles cannot work.
        const bool pair_is_split_across_tiles = iserdes_bel.tile != oserdes_bel.tile;
        if (pair_is_split_across_tiles)
            log_error("OFB loopback pair '%s'/'%s' is pinned into two tiles ('%s' and '%s')\n", ctx->nameOf(ci),
                      ctx->nameOf(iserdes), ctx->nameOfBel(oserdes_bel), ctx->nameOfBel(iserdes_bel));
        const bool iserdes_bel_is_taken = !ctx->checkBelAvail(iserdes_bel);
        if (iserdes_bel_is_taken)
            log_error("ISERDESE2 '%s' cannot take bel '%s': '%s' holds it\n", ctx->nameOf(iserdes),
                      ctx->nameOfBel(iserdes_bel), ctx->nameOf(ctx->getBoundBelCell(iserdes_bel)));
        ctx->bindBel(iserdes_bel, iserdes, STRENGTH_LOCKED);
        log_info("   binding OFB loopback pair '%s'/'%s' to bels '%s' and '%s'\n", ctx->nameOf(ci),
                 ctx->nameOf(iserdes), ctx->nameOfBel(oserdes_bel), ctx->nameOfBel(iserdes_bel));
    }

    flush_cells();
    generic_xform(iologic_rules, false);
    flush_cells();
}

SiteIndex XC7Packer::get_ilogic_site_for_ologic(SiteIndex ologic_site)
{
    const auto &sites = uarch->tile_extra_data(ologic_site.tile)->sites;
    const auto &odata = sites[ologic_site.site];
    for (int32_t i = 0; i < int32_t(sites.ssize()); i++) {
        const auto &s = sites[i];
        bool is_sibling_ilogic = boost::starts_with(IdString(s.name_prefix).str(ctx), "ILOGIC") &&
                                 s.site_x == odata.site_x && s.site_y == odata.site_y;
        if (is_sibling_ilogic)
            return SiteIndex(ologic_site.tile, i);
    }
    NPNR_ASSERT_FALSE("failed to find sibling ILOGIC site for OSERDESE2");
}

SiteIndex XC7Packer::get_ologic_site_for_ilogic(SiteIndex ilogic_site)
{
    const auto &sites = uarch->tile_extra_data(ilogic_site.tile)->sites;
    const auto &idata = sites[ilogic_site.site];
    for (int32_t i = 0; i < int32_t(sites.ssize()); i++) {
        const auto &s = sites[i];
        bool is_sibling_ologic = boost::starts_with(IdString(s.name_prefix).str(ctx), "OLOGIC") &&
                                 s.site_x == idata.site_x && s.site_y == idata.site_y;
        if (is_sibling_ologic)
            return SiteIndex(ilogic_site.tile, i);
    }
    NPNR_ASSERT_FALSE("failed to find sibling OLOGIC site for ISERDESE2");
}

// Several IDELAYCTRLs, as when two interfaces each bring their own, become
// one: the per-region copies made below serve every I/ODELAY anyway.  They
// must share a REFCLK (telling them apart would need IODELAY_GROUP, which is
// not supported); differing resets are ORed, so any of them resets the
// copies, and every RDY is the AND of all the copies.  The first cell stands
// for the others, whose names are kept in X_MERGED_IDELAYCTRLS for the
// placement dump.
void XC7Packer::merge_idelayctrls(const std::vector<CellInfo *> &idelayctrls)
{
    CellInfo *keep = idelayctrls.front();
    NetInfo *refclk = keep->getPort(id_REFCLK);
    for (CellInfo *other : idelayctrls) {
        const bool refclk_differs = other->getPort(id_REFCLK) != refclk;
        if (refclk_differs)
            log_error("IDELAYCTRL cells '%s' and '%s' have different REFCLK nets; several IDELAYCTRLs are only "
                      "supported on a shared REFCLK (IODELAY_GROUP is not supported)\n",
                      ctx->nameOf(keep), ctx->nameOf(other));
    }

    // A reset tied low resets nothing, so it does not take part in the OR.
    std::vector<NetInfo *> resets;
    for (CellInfo *ci : idelayctrls) {
        NetInfo *rst = ci->getPort(id_RST);
        const bool resets_something = rst != nullptr && rst->name != ctx->id("$PACKER_GND_NET");
        const bool reset_is_new = resets_something && std::find(resets.begin(), resets.end(), rst) == resets.end();
        if (reset_is_new)
            resets.push_back(rst);
    }
    NetInfo *merged_rst = resets.empty() ? nullptr : resets.front();
    for (size_t j = 1; j < resets.size(); j++) {
        NetInfo *ored = create_internal_net(keep->name, "MERGED_RST_" + std::to_string(j), false);
        create_lut(keep->name.str(ctx) + "/RST_OR_LUT_" + std::to_string(j), {merged_rst, resets.at(j)}, ored,
                   Property(14));
        merged_rst = ored;
    }
    const bool keep_needs_merged_rst = merged_rst != nullptr && keep->getPort(id_RST) != merged_rst;
    if (keep_needs_merged_rst) {
        keep->disconnectPort(id_RST);
        keep->connectPort(id_RST, merged_rst);
    }

    NetInfo *rdy = keep->getPort(id_RDY);
    std::vector<std::string> merged_names;
    for (CellInfo *other : idelayctrls) {
        const bool is_the_kept_one = other == keep;
        if (is_the_kept_one)
            continue;
        merged_names.push_back(other->name.str(ctx));
        NetInfo *other_rdy = other->getPort(id_RDY);
        other->disconnectPort(id_RDY);
        const bool nothing_to_move = other_rdy == nullptr || other_rdy == rdy;
        if (nothing_to_move) {
            // nothing to move
        } else {
            const bool keep_has_no_rdy = rdy == nullptr;
            if (keep_has_no_rdy) {
                keep->connectPort(id_RDY, other_rdy);
                rdy = other_rdy;
            } else {
                std::vector<PortRef> users;
                for (const PortRef &usr : other_rdy->users)
                    users.push_back(usr);
                for (const PortRef &usr : users) {
                    usr.cell->disconnectPort(usr.port);
                    usr.cell->connectPort(usr.port, rdy);
                }
            }
        }
        other->disconnectPort(id_REFCLK);
        other->disconnectPort(id_RST);
        packed_cells.insert(other->name);
    }
    keep->attrs[ctx->id("X_MERGED_IDELAYCTRLS")] = boost::algorithm::join(merged_names, " ");
    log_info("Merged %d IDELAYCTRL cells into '%s'\n", int(idelayctrls.size()), ctx->nameOf(keep));
}

void XC7Packer::pack_idelayctrl()
{
    std::vector<CellInfo *> idelayctrls;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        const bool is_idelayctrl = ci->type == id_IDELAYCTRL;
        if (is_idelayctrl)
            idelayctrls.push_back(ci);
    }
    const bool no_idelayctrl = idelayctrls.empty();
    if (no_idelayctrl)
        return;
    const bool several_idelayctrls = idelayctrls.size() > 1;
    if (several_idelayctrls) {
        merge_idelayctrls(idelayctrls);
        flush_cells();
    }
    CellInfo *idelayctrl = idelayctrls.front();
    std::set<SiteIndex> ioctrl_sites;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type.in(id_IDELAYE2_IDELAYE2, id_ODELAYE2_ODELAYE2)) {
            if (ci->bel == BelId())
                continue;
            ioctrl_sites.insert(get_ioctrl_site(ctx->getBelByNameStr(ci->attrs.at(id_X_IO_BEL).as_string())));
        }
    }
    const bool design_has_no_iodelays = ioctrl_sites.empty();
    if (design_has_no_iodelays) {
        // An IDELAYCTRL in a design with no I/ODELAYs is useless but legal --
        // Vivado places it and drives RDY rather than rejecting the design, and a
        // design can legitimately arrive in that state after optimisation removed
        // the last delay element. Warn, rename the cell so it still gets placed,
        // and stop here. (port of nextpnr-xilinx 06769c05)
        //
        // The early return is load-bearing, not tidiness: falling through would
        // leave dup_rdys empty, and the RDY-AND tree below calls dup_rdys.front().
        log_warning("Found IDELAYCTRL '%s' but no I/ODELAYs; leaving it unreplicated\n",
                    ctx->nameOf(idelayctrl));
        ioctrl_rules[id_IDELAYCTRL].new_type = id_IDELAYCTRL_IDELAYCTRL;
        generic_xform(ioctrl_rules);
        return;
    }
    NetInfo *rdy = idelayctrl->getPort(id_RDY);
    idelayctrl->disconnectPort(id_RDY);
    std::vector<NetInfo *> dup_rdys;
    int i = 0;
    for (auto site : ioctrl_sites) {
        CellInfo *dup_idc =
                create_cell(id_IDELAYCTRL, int_name(idelayctrl->name, "CTRL_DUP_" + std::to_string(i), false));
        dup_idc->connectPort(id_REFCLK, idelayctrl->getPort(id_REFCLK));
        dup_idc->connectPort(id_RST, idelayctrl->getPort(id_RST));
        const bool has_merged_names = idelayctrl->attrs.count(ctx->id("X_MERGED_IDELAYCTRLS"));
        if (has_merged_names)
            dup_idc->attrs[ctx->id("X_MERGED_IDELAYCTRLS")] = idelayctrl->attrs.at(ctx->id("X_MERGED_IDELAYCTRLS"));
        if (rdy) {
            NetInfo *dup_rdy =
                    (ioctrl_sites.size() == 1)
                            ? rdy
                            : create_internal_net(idelayctrl->name, "CTRL_DUP_" + std::to_string(i) + "_RDY", false);
            dup_idc->connectPort(id_RDY, dup_rdy);
            dup_rdys.push_back(dup_rdy);
        }
        BelId idc_bel = uarch->get_site_bel(site, id_IDELAYCTRL);
        NPNR_ASSERT(idc_bel != BelId());
        ctx->bindBel(idc_bel, dup_idc, STRENGTH_LOCKED);
        ++i;
    }
    idelayctrl->disconnectPort(id_REFCLK);
    idelayctrl->disconnectPort(id_RST);

    if (rdy != nullptr) {
        // AND together all the RDY signals
        std::vector<NetInfo *> int_anded_rdy;
        int_anded_rdy.push_back(dup_rdys.front());
        for (size_t j = 1; j < dup_rdys.size(); j++) {
            NetInfo *anded_net =
                    (j == (dup_rdys.size() - 1))
                            ? rdy
                            : create_internal_net(idelayctrl->name, "ANDED_RDY_" + std::to_string(j), false);
            create_lut(idelayctrl->name.str(ctx) + "/RDY_AND_LUT_" + std::to_string(j),
                       {int_anded_rdy.at(j - 1), dup_rdys.at(j)}, anded_net, Property(8));
            int_anded_rdy.push_back(anded_net);
        }
    }

    packed_cells.insert(idelayctrl->name);
    flush_cells();

    ioctrl_rules[id_IDELAYCTRL].new_type = id_IDELAYCTRL_IDELAYCTRL;

    generic_xform(ioctrl_rules);
}

// The BSCAN bel of every site that serves a user JTAG chain, keyed by the
// chain number.
//
// USER<n> is served by site BSCAN_X0Y<n-1>.  The site is not just an address
// the cell could have taken: its pins *are* that chain's SEL, CAPTURE, SHIFT
// and DRCK wires, so a BSCAN bound elsewhere sees the other chain, however
// its JTAG_CHAIN parameter reads -- that parameter only drives the
// JTAG_CHAIN_<n> bit, a device-wide configuration, not the routing.
//
// A site's name is what says which chain it serves, and the name comes from
// the architecture rather than from text matched against a bel name: this is
// a lookup of the site serving a chain, so a device database whose names do
// not match must give an error, not leave the instance to be placed anywhere.
dict<int, BelId> XC7Packer::bscan_bels_by_chain()
{
    const std::string bscan_site_prefix = "BSCAN_X0Y";
    dict<int, BelId> by_chain;
    for (BelId bel : ctx->getBels()) {
        bool is_bscan_bel = ctx->getBelType(bel) == id_BSCAN;
        if (!is_bscan_bel)
            continue;
        std::string site_name = uarch->get_site_name(uarch->get_bel_site(bel)).str(ctx);
        bool is_bscan_site = boost::starts_with(site_name, bscan_site_prefix);
        if (!is_bscan_site)
            continue;
        // BSCAN_X0Y<chain-1> serves USER<chain>.
        int chain = std::stoi(site_name.substr(bscan_site_prefix.size())) + 1;
        by_chain[chain] = bel;
    }
    return by_chain;
}

void XC7Packer::pack_cfg()
{
    log_info("Packing cfg...\n");
    dict<IdString, XFormRule> cfg_rules;
    cfg_rules[id_BSCANE2].new_type = id_BSCAN;
    cfg_rules[id_DCIRESET].new_type = id_DCIRESET_DCIRESET;
    cfg_rules[id_DNA_PORT].new_type = id_DNA_PORT_DNA_PORT;
    cfg_rules[id_EFUSE_USR].new_type = id_EFUSE_USR_EFUSE_USR;
    cfg_rules[id_ICAPE2].new_type = id_ICAP_ICAP;
    cfg_rules[id_FRAME_ECCE2].new_type = id_FRAME_ECC_FRAME_ECC;
    cfg_rules[id_STARTUPE2].new_type = id_STARTUP_STARTUP;
    cfg_rules[id_USR_ACCESSE2].new_type = id_USR_ACCESS_USR_ACCESS;
    generic_xform(cfg_rules);

    // Read once: which site serves which chain is a property of the device,
    // not of any one instance.
    dict<int, BelId> bscan_bel_for_chain = bscan_bels_by_chain();

    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->type == id_BSCAN) {
            int chain = int_or_default(ci->params, id_JTAG_CHAIN, 1);
            bool chain_in_range = chain >= 1 && chain <= 4;
            if (!chain_in_range)
                log_error("Instance '%s': Invalid JTAG_CHAIN number of '%d'. Allowed values are: 1-4.\n",
                          ci->name.c_str(ctx), chain);
            // Pin the instance to the site its chain selects, unless the
            // design pinned the bel itself.
            bool design_left_the_bel_unset = !ci->attrs.count(id_BEL) && ci->bel == BelId();
            if (design_left_the_bel_unset) {
                auto found = bscan_bel_for_chain.find(chain);
                bool device_has_no_site_for_this_chain = found == bscan_bel_for_chain.end();
                if (device_has_no_site_for_this_chain)
                    log_error("Instance '%s': this device has no BSCAN site for JTAG_CHAIN %d.\n",
                              ci->name.c_str(ctx), chain);
                BelId chain_bel = found->second;
                bool site_is_taken_by_another_cell = !ctx->checkBelAvail(chain_bel);
                if (site_is_taken_by_another_cell)
                    log_error("Instance '%s': site %s for JTAG_CHAIN %d is taken by '%s'.\n", ci->name.c_str(ctx),
                              ctx->nameOfBel(chain_bel), chain, ctx->nameOf(ctx->getBoundBelCell(chain_bel)));
                ctx->bindBel(chain_bel, ci, STRENGTH_LOCKED);
            }
        }
        // These configuration primitives each live in a single dedicated
        // site; the placer cannot discover that site on its own, so without
        // preplacement it aborts with "Unable to find legal placement for
        // cell".  (Port of nextpnr-xilinx d42d6c9b.)
        // A BSCAN is not one of them: it is bound to the site its JTAG_CHAIN
        // selects, above -- leaving it to take any free BSCAN site is the bug
        // that binding exists to prevent.
        bool is_single_site_config_primitive =
                ci->type.in(id_DCIRESET_DCIRESET, id_DNA_PORT_DNA_PORT, id_EFUSE_USR_EFUSE_USR, id_ICAP_ICAP,
                            id_FRAME_ECC_FRAME_ECC, id_STARTUP_STARTUP, id_USR_ACCESS_USR_ACCESS);
        if (is_single_site_config_primitive)
            preplace_unique(ci);
    }
}


SiteIndex XC7Packer::get_gt_site(BelId pad_bel, IdString want)
{
    int tile = pad_bel.tile;
    const auto &sites = uarch->tile_extra_data(tile)->sites;
    for (int32_t i = 0; i < int32_t(sites.ssize()); i++) {
        // A GT tile holds a COMMON site and a CHANNEL site, and both are
        // named GTxE2_something.  Match the site's KIND to the cell's type --
        // a GTXE2_CHANNEL belongs in a GTXE2_CHANNEL site -- rather than
        // taking the first GT-prefixed site and hoping.  That was harmless
        // while only a COMMON was ever placed this way; once a CHANNEL is
        // placed too, first-match is the wrong site.  The site name and the
        // cell type are the same spelling, which is what makes this exact
        // rather than a heuristic, and it does not depend on the order the
        // sites happen to be in.
        if (IdString(sites[i].name_prefix) != want)
            continue;
        return SiteIndex(tile, i);
    }
    // Not an error: a COMMON shares the reference clock pad's tile, but a
    // CHANNEL taking that clock directly (CPLL mode) sits in a tile of its
    // own, located by its own data pads.  The caller leaves it to the placer.
    return SiteIndex();
}

void XC7Packer::constrain_ibufds_gt_site(CellInfo *buf_cell, BelId pad_bel)
{
    // Port of nextpnr-xilinx constrain_ibufds_gt_site: the IBUFDS_GTE2 site
    // is hardwired to the GT pad pair it buffers (GTREFCLK0 = lower buffer,
    // GTREFCLK1 = upper), so compute the y from the pad's position among the
    // tile's four GT pads.
    //
    // Paired on the SITE coordinates, not the tile-relative ones.  rel_y is a
    // geometric row within the tile and does not run in the same order as the
    // sites' own numbering: in a GTX_COMMON the pad named IPAD_X0Y0 has
    // rel_y = 2, so pairing on rel_y sent the reference clock for pads Y0/Y1
    // through buffer Y1 -- the half the clock does not arrive at, which is a
    // transceiver with no reference clock and a PHY whose MMCMs never lock.
    // Pads and buffers are numbered together in their names, and that is the
    // pairing the silicon and Vivado both use.
    int tile = pad_bel.tile;
    const auto &sites = uarch->tile_extra_data(tile)->sites;
    // Find the pad's site by asking which site owns this bel, rather than by
    // reading the bel's own site index.  In a GTX_COMMON the two disagree: the
    // sites array is not ordered by coordinate (its IPADs run Y10, Y11, Y8,
    // Y9) and the back-pointer on a pad bel lands on the wrong entry, so the
    // pad for package pin AH8 -- IPAD_X2Y8, MGTREFCLK0 -- was read as
    // IPAD_X2Y10, MGTREFCLK1.  Everything downstream then picked the buffer
    // for the reference clock the board does not drive.
    SiteIndex pad_site;
    for (int32_t i = 0; i < int32_t(sites.ssize()); i++) {
        bool site_holds_this_pad = uarch->get_site_bel(SiteIndex(tile, i), ctx->id("PAD")) == pad_bel;
        if (site_holds_this_pad) {
            pad_site = SiteIndex(tile, i);
            break;
        }
    }
    if (pad_site == SiteIndex())
        log_error("failed to find the site holding GT pad '%s'\n", ctx->nameOfBel(pad_bel));
    int32_t min_pad_y = INT_MAX, max_pad_y = 0, pad_y = -1;
    int32_t min_buf_y = INT_MAX, max_buf_y = 0, buf_x = -1;
    for (int32_t i = 0; i < int32_t(sites.ssize()); i++) {
        const auto &s = sites[i];
        std::string name = IdString(s.name_prefix).str(ctx);
        if ((name == "IPAD")) {
            int32_t sy = s.site_y;
            if (sy < min_pad_y) min_pad_y = sy;
            if (max_pad_y < sy) max_pad_y = sy;
            if (i == pad_site.site) pad_y = sy;
        }
        if ((name == "IBUFDS_GTE2")) {
            int32_t sy = s.site_y;
            if (sy < min_buf_y) min_buf_y = sy;
            if (max_buf_y < sy) max_buf_y = sy;
            if (buf_x < 0) buf_x = s.site_x;
        }
    }
    if (pad_y < 0)
        log_error("failed to find IBUFDS_GTE2 site for pad '%s'\n", ctx->nameOfBel(pad_bel));
    NPNR_ASSERT(min_pad_y < max_pad_y);
    NPNR_ASSERT(min_buf_y < max_buf_y);
    NPNR_ASSERT(buf_x >= 0);

    int32_t rel_buf_y = (pad_y - min_pad_y) >> 1;
    int32_t buf_y = min_buf_y + rel_buf_y;


    SiteIndex buf_site;
    for (int32_t i = 0; i < int32_t(sites.ssize()); i++) {
        const auto &s = sites[i];
        std::string name = IdString(s.name_prefix).str(ctx);
        bool is_matching_refclk_buffer = name == "IBUFDS_GTE2" && s.site_x == buf_x && s.site_y == buf_y;
        if (is_matching_refclk_buffer) {
            buf_site = SiteIndex(tile, i);
            break;
        }
    }
    NPNR_ASSERT(buf_site != SiteIndex());
    BelId buf_bel = uarch->get_site_bel(buf_site, id_IBUFDS_GTE2);
    NPNR_ASSERT(buf_bel != BelId());
    ctx->bindBel(buf_bel, buf_cell, STRENGTH_LOCKED);
    buf_cell->params[ctx->id("_REL_BUF_Y")] = Property(rel_buf_y);
    log_info("    Constraining '%s' to site '%s'\n", buf_cell->name.c_str(ctx),
             uarch->get_site_name(buf_site).c_str(ctx));
}

void XC7Packer::constrain_gt(CellInfo *pad_cell, CellInfo *gt_cell)
{
    BelId pad_bel;
    if (pad_cell->attrs.count(id_BEL))
        pad_bel = ctx->getBelByNameStr(pad_cell->attrs.at(id_BEL).as_string());
    if (pad_bel == BelId())
        log_error("Pad cell %s has not been placed\n", pad_cell->name.c_str(ctx));

    if (gt_cell->bel != BelId()) {
        bool gt_placed_in_another_tile = gt_cell->bel.tile != pad_bel.tile;
        if (gt_placed_in_another_tile) {
            // A different tile is not automatically a conflict.  A reference
            // clock pad feeds an IBUFDS_GTE2 whose output is distributed to
            // transceivers across the quad, so sgmii_refclk_p legitimately
            // sits in one tile while the GTXE2_CHANNEL it clocks sits in
            // another -- and by the time this pad is reached, that channel has
            // already been placed by its own data pads.  Only complain if this
            // pad's tile actually offers a site for this cell type, which is
            // the case where the two placements really do disagree.  The
            // unplaced path below already reasons this way.
            bool pad_tile_has_no_site_for_gt = get_gt_site(pad_bel, gt_cell->type) == SiteIndex();
            if (pad_tile_has_no_site_for_gt) {
                log_info("    '%s' takes its reference clock from pad '%s' in another tile; "
                         "keeping its existing placement\n",
                         gt_cell->name.c_str(ctx), pad_cell->name.c_str(ctx));
                return;
            }
            log_error("Location of pad %s on tile %d conflicts with previous placement of %s on tile %d\n",
                      pad_cell->name.c_str(ctx), pad_bel.tile, gt_cell->name.c_str(ctx), gt_cell->bel.tile);
        }
        return;
    }
    SiteIndex gt_site = get_gt_site(pad_bel, gt_cell->type);
    if (gt_site == SiteIndex()) {
        // The reference clock reaches a transceiver that is not in this pad's
        // tile.  Pinning it here would be wrong even if a site existed; say so
        // and let the placer put it where its own pads require.
        log_info("    '%s' takes its reference clock from pad '%s' but sits in another tile; "
                 "leaving it to the placer\n",
                 gt_cell->name.c_str(ctx), pad_cell->name.c_str(ctx));
        return;
    }
    BelId gt_bel = uarch->get_site_bel(gt_site, gt_cell->type);
    NPNR_ASSERT(gt_bel != BelId());
    ctx->bindBel(gt_bel, gt_cell, STRENGTH_LOCKED);
    log_info("    Constraining '%s' to site '%s'\n", gt_cell->name.c_str(ctx),
             uarch->get_site_name(gt_site).c_str(ctx));
}

void XC7Packer::pack_gt()
{
    // Port of nextpnr-xilinx pack_gt_xc7.cc
    log_info("Packing Gigabit Transceivers..\n");

    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        bool is_gt_common = ci->type == id_GTPE2_COMMON || ci->type == id_GTXE2_COMMON;
        bool is_gt_channel = ci->type == id_GTPE2_CHANNEL || ci->type == id_GTXE2_CHANNEL;

        if (is_gt_common) {
            const IdString refclk0_used_attr = ctx->id("_GTREFCLK0_USED"),
                           refclk1_used_attr = ctx->id("_GTREFCLK1_USED");
            bool refclk0_used = false, refclk1_used = false;
            bool is_gtp = ci->type == id_GTPE2_COMMON;
            std::vector<std::pair<IdString, IdString>> to_rename;

            fold_inverter(ci, "DRPCLK");
            if (is_gtp) {
                fold_inverter(ci, "PLL0LOCKDETCLK");
                fold_inverter(ci, "PLL1LOCKDETCLK");
            } else {
                fold_inverter(ci, "QPLLLOCKDETCLK");
            }

            for (auto &port : ci->ports) {
                std::string port_name = port.first.str(ctx);
                NetInfo *port_net = port.second.net;
                bool used = port_net != nullptr && port_net->name != ctx->id("$PACKER_VCC_NET") &&
                            port_net->name != ctx->id("$PACKER_GND_NET");
                bool internal_refclk = false;

                // GTEAST/GTWESTREFCLK0/1 (GTP) and GTNORTH/GTSOUTHREFCLK0/1 (GTX)
                // are dedicated clock inputs from a neighbouring quad.
                const bool is_neighbour_refclk = boost::starts_with(port_name, "GTEASTREFCLK") ||
                                                 boost::starts_with(port_name, "GTWESTREFCLK") ||
                                                 boost::starts_with(port_name, "GTNORTHREFCLK") ||
                                                 boost::starts_with(port_name, "GTSOUTHREFCLK");
                const bool tied_to_constant_net =
                        port_net != nullptr &&
                        (port_net->name == ctx->id("$PACKER_GND_NET") || port_net->name == ctx->id("$PACKER_VCC_NET"));
                const bool neighbour_refclk_tied_to_constant = is_neighbour_refclk && tied_to_constant_net;

                if (port_name == "DRPCLK") {
                    ci->params[ctx->id("_DRPCLK_USED")] = Property(used);
                } else if (neighbour_refclk_tied_to_constant) {
                    // The Vivado wizard ties these inputs to ground. They have
                    // no route from the fabric, so leave them unconnected, as
                    // for a GTREFCLK tied to a constant.
                    ci->disconnectPort(port.first);
                } else if (boost::starts_with(port_name, "GTREFCLK")) {
                    if (port_net == nullptr)
                        continue;
                    CellInfo *driver = port_net->driver.cell;
                    if (driver == nullptr)
                        log_error("Port %s connected to net %s has no driver!\n", port_name.c_str(),
                                  port_net->name.c_str(ctx));
                    bool refclk_tied_to_constant =
                            !used || driver->type == id_PSEUDO_GND || driver->type == id_PSEUDO_VCC;
                    if (refclk_tied_to_constant) {
                        // unused, not a clock
                        ci->disconnectPort(port.first);
                        continue;
                    }
                    if (driver->type != id_IBUFDS_GTE2) {
                        if (driver->type != id_BUFGCTRL)
                            log_error("%s COMMON GTREFCLK connected to unsupported cell type %s\n",
                                      is_gtp ? "GTPE2" : "GTXE2", driver->type.c_str(ctx));
                        // Vivado internally always connects to GTGREFCLK0
                        auto gtg_port = is_gtp ? id_GTGREFCLK0 : id_GTGREFCLK;
                        log_warning("Internal REFCLK is used for instance '%s', which is not recommended. "
                                    "Connecting refclock to port %s instead.\n",
                                    ci->name.c_str(ctx), gtg_port.c_str(ctx));
                        to_rename.emplace_back(port.first, gtg_port);
                        internal_refclk = true;
                        ci->params[ctx->id("_GTGREFCLK_USED")] = Property(1, 1);
                    } else { // driver is IBUFDS_GTE2
                        if (used) {
                            int64_t rel_buf_y = ci->params.count(ctx->id("_REL_BUF_Y"))
                                                        ? 0
                                                        : int_or_default(driver->params, ctx->id("_REL_BUF_Y"), 0);
                            // GTREFCLK0 is hardwired to the lower IBUFDS_GTE2,
                            // GTREFCLK1 to the upper: disconnect (no routing
                            // needed) and mark the used input
                            ci->disconnectPort(port.first);
                            if (rel_buf_y == 1) {
                                refclk1_used = true;
                                ci->params[refclk1_used_attr] = Property(1, 1);
                            } else {
                                refclk0_used = true;
                                ci->params[refclk0_used_attr] = Property(1, 1);
                            }
                            continue;
                        }
                    }
                    if (!internal_refclk) {
                        if (boost::ends_with(port_name, "0")) {
                            refclk0_used = used;
                            ci->params[refclk0_used_attr] = Property(used);
                        } else {
                            refclk1_used = used;
                            ci->params[refclk1_used_attr] = Property(used);
                        }
                    }
                }
            }
            for (auto [from, to] : to_rename)
                ci->renamePort(from, to);
            ci->params[ctx->id("_BOTH_GTREFCLK_USED")] = Property(refclk0_used && refclk1_used);
        } else if (is_gt_channel) {
            bool is_gtp = ci->type == id_GTPE2_CHANNEL;
            for (auto p : {"CLKRSVD0", "CLKRSVD1", "CPLLLOCKDETCLK", "DMONITORCLK", "DRPCLK", "GTGREFCLK",
                           "PMASCANCLK0", "PMASCANCLK1", "PMASCANCLK2", "PMASCANCLK3", "QPLLLOCKDETCLK", "RXUSRCLK",
                           "RXUSRCLK2", "SCANCLK", "SIGVALIDCLK", "TSTCLK0", "TSTCLK1", "TXPHDLYTSTCLK", "TXUSRCLK",
                           "TXUSRCLK2"})
                fold_inverter(ci, p);

            // Collect bracket-named ports first to avoid iterator
            // invalidation when renamePort erases+inserts
            std::vector<std::pair<IdString, IdString>> to_rename;
            for (auto &port : ci->ports) {
                std::string port_name = port.first.str(ctx);
                NetInfo *net = ci->getPort(port.first);

                bool is_pll_clock_port = boost::starts_with(port_name, "PLL") && boost::ends_with(port_name, "CLK");
                bool is_refclk_or_qpll_port =
                        boost::contains(port_name, "REFCLK") || boost::starts_with(port_name, "QPLL");
                bool is_clock_status_port =
                        boost::contains(port_name, "CLKMONITOR") || boost::contains(port_name, "CLKLOST");
                bool is_hardwired_clock_input =
                        net != nullptr && (is_pll_clock_port || is_refclk_or_qpll_port) && !is_clock_status_port;
                if (is_hardwired_clock_input) {
                    bool tied_to_constant_net =
                            net->name == ctx->id("$PACKER_GND_NET") || net->name == ctx->id("$PACKER_VCC_NET");
                    if (tied_to_constant_net) {
                        ci->disconnectPort(port.first);
                        continue;
                    }
                    CellInfo *driver = net->driver.cell;
                    bool has_no_real_driver =
                            driver == nullptr || driver->type == id_PSEUDO_GND || driver->type == id_PSEUDO_VCC;
                    if (has_no_real_driver) {
                        ci->disconnectPort(port.first);
                        continue;
                    }
                    IdString common_type = is_gtp ? id_GTPE2_COMMON : id_GTXE2_COMMON;
                    // GTX CPLL mode: GTXE2_CHANNEL.GTREFCLK* may be driven
                    // directly by IBUFDS_GTE2 (no COMMON in the design)
                    bool driven_by_common_or_refclk_buffer =
                            driver->type == common_type || driver->type == id_IBUFDS_GTE2;
                    if (!driven_by_common_or_refclk_buffer)
                        log_error("The clock port '%s' of the %s instance %s can only be driven by the clock "
                                  "outputs of a %s or an IBUFDS_GTE2 instance, but not %s\n",
                                  port_name.c_str(), is_gtp ? "GTPE2_CHANNEL" : "GTXE2_CHANNEL",
                                  ci->name.c_str(ctx), common_type.c_str(ctx), driver->type.c_str(ctx));
                    if (driver->type == common_type) {
                        auto drv_port = net->driver.port.str(ctx);
                        auto port_prefix = port_name.substr(0, 4);
                        auto port_suffix = port_name.substr(4);
                        bool driver_port_matches =
                                boost::starts_with(drv_port, port_prefix) && boost::ends_with(drv_port, port_suffix);
                        if (!driver_port_matches)
                            log_error("The port %s of a %s instance can only be connected to the port %sOUT%s "
                                      "of a %s instance, but not to %s.\n",
                                      port_name.c_str(), is_gtp ? "GTPE2_CHANNEL" : "GTXE2_CHANNEL",
                                      port_prefix.c_str(), port_suffix.c_str(), common_type.c_str(ctx),
                                      drv_port.c_str());
                    }
                    bool used = net->name != ctx->id("$PACKER_VCC_NET") && net->name != ctx->id("$PACKER_GND_NET");
                    bool refclk0_used = used && port_name == "GTREFCLK0";
                    bool refclk1_used = used && port_name == "GTREFCLK1";
                    if (refclk0_used)
                        ci->params[ctx->id("_GTREFCLK0_USED")] = Property(1, 1);
                    if (refclk1_used)
                        ci->params[ctx->id("_GTREFCLK1_USED")] = Property(1, 1);
                    // hardwired; disconnect
                    ci->disconnectPort(port.first);
                }
                bool port_name_has_brackets = boost::contains(port_name, "[") && boost::contains(port_name, "]");
                if (port_name_has_brackets) {
                    std::string new_port_name = port_name;
                    boost::replace_all(new_port_name, "[", "");
                    boost::replace_all(new_port_name, "]", "");
                    to_rename.emplace_back(ctx->id(port_name), ctx->id(new_port_name));
                }
            }
            for (auto &pr : to_rename)
                ci->renamePort(pr.first, pr.second);
        }
    }
}

NEXTPNR_NAMESPACE_END
