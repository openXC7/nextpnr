void XilinxImpl::pack()
{
    const ArchArgs &args = ctx->args;
    if (args.options.count("xdc")) {
        parse_xdc(args.options["xdc"].as<std::string>());
    }

    XC7Packer packer(ctx, this);
    packer.pack_constants();
    packer.pack_inverters();
    packer.pack_io();
    packer.prepare_clocking();
    packer.pack_constants();
    packer.pack_iologic();
    packer.pack_idelayctrl();
    packer.pack_clocking();
    packer.generate_constraints();
    packer.pack_muxfs();
    packer.pack_carries();
    packer.pack_srls();
    packer.pack_luts();
    packer.pack_dram();
    packer.pack_bram();
    packer.pack_dsps();
    packer.pack_ffs();
    packer.finalise_muxfs();
    packer.pack_lutffs();
}
