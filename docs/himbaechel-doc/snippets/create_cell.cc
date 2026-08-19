        IdString id = ctx->id(name);
        cell->ports[id].name = id;
        cell->ports[id].type = dir;
    };
    if (type == id_SLICE_LUTX) {
        for (int i = 1; i <= 6; i++)
            add_port("A" + std::to_string(i), PORT_IN);
        for (int i = 1; i <= 9; i++)
            add_port("WA" + std::to_string(i), PORT_IN);
        add_port("DI1", PORT_IN);
        add_port("DI2", PORT_IN);
        add_port("CLK", PORT_IN);
        add_port("WE", PORT_IN);
        add_port("SIN", PORT_IN);
        add_port("O5", PORT_OUT);
        add_port("O6", PORT_OUT);
        add_port("MC31", PORT_OUT);
    } else if (type == id_SLICE_FFX) {
        add_port("D", PORT_IN);
        add_port("SR", PORT_IN);
        add_port("CE", PORT_IN);
        add_port("CLK", PORT_IN);
        add_port("Q", PORT_OUT);
    } else if (type == id_RAMD64E) {
        for (int i = 0; i < 6; i++)
