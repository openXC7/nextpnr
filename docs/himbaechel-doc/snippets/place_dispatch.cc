bool Arch::place()
{
    bool retVal = false;
    uarch->prePlace();
    std::string placer = str_or_default(settings, id("placer"), defaultPlacer);
    if (placer == "heap") {
        PlacerHeapCfg cfg(getCtx());
        uarch->configurePlacerHeap(cfg);
        cfg.ioBufTypes.insert(id("GENERIC_IOB"));
        retVal = placer_heap(getCtx(), cfg);
    } else if (placer == "static") {
        PlacerStaticCfg cfg(getCtx());
        uarch->configurePlacerStatic(cfg);
        retVal = placer_static(getCtx(), cfg);
    } else if (placer == "sa") {
        retVal = placer1(getCtx(), Placer1Cfg(getCtx()));
    } else {
        log_error("Himbächel architecture does not support placer '%s'\n", placer.c_str());
    }
    uarch->postPlace();
    getCtx()->settings[getCtx()->id("place")] = 1;
    archInfoToAttributes();
    return retVal;
}
