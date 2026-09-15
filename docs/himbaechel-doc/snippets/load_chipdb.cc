void Arch::load_chipdb(const std::string &path)
{
    std::string db_path;
    if (!args.chipdb_override.empty()) {
        db_path = args.chipdb_override;
    } else {
        db_path = proc_share_dirname();
        db_path += "himbaechel/";
        db_path += path;
        std::filesystem::path p(db_path);
        db_path = p.make_preferred().string();
    }
    try {
        blob_file.open(db_path);
        if (db_path.empty() || !blob_file.is_open())
            log_error("Unable to read chipdb %s\n", db_path.c_str());
        const char *blob = reinterpret_cast<const char *>(blob_file.data());
        chip_info = get_chip_info(reinterpret_cast<const RelPtr<ChipInfoPOD> *>(blob));
    } catch (...) {
        log_error("Unable to read chipdb %s\n", db_path.c_str());
    }
    // Check consistency of blob
    if (chip_info->magic != 0x00ca7ca7)
        log_error("chipdb %s does not look like a valid himbächel database!\n", db_path.c_str());
    if (chip_info->version != database_version)
        log_error(
                "chipdb uses db version %d but nextpnr is expecting version %d (did you forget a database rebuild?).\n",
                chip_info->version, database_version);
    std::string blob_uarch(chip_info->uarch.get());
    if (blob_uarch != args.uarch)
        log_error("database device uarch '%s' does not match selected device uarch '%s'.\n", blob_uarch.c_str(),
                  args.uarch.c_str());
    // Setup constids from database
    for (int i = 0; i < chip_info->extra_constids->bba_ids.ssize(); i++) {
        IdString::initialize_add(this, chip_info->extra_constids->bba_ids[i].get(),
                                 i + chip_info->extra_constids->known_id_count);
    }
}
