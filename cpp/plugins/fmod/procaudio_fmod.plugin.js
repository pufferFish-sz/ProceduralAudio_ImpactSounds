/* FMOD Studio deck layout for the "ProcAudio Contact" plug-in instrument.
   Must sit next to procaudio_fmod.dll in a Studio plug-in folder. Without
   it Studio shows every parameter as a dial (FMOD Studio 2.02 Plug-in
   Scripting API: registerPluginDescription, deckWidgetType.Dropdown).
   Studio titles each bound control itself, so no separate Label widgets. */

studio.plugins.registerPluginDescription("ProcAudio Contact", {
    companyName: "ProcAudio",
    productName: "Contact",
    parameters: {
        "Object": {
            displayName: "Object",
            enumeration: ["Wood ball S", "Wood ball L", "Steel ball S", "Steel ball L",
                          "Card box S", "Card box L", "Steel box S", "Steel box L"],
        },
        "Surface": {
            displayName: "Surface",
            enumeration: ["Varnished wood", "Wood floor", "Concrete", "Tile", "Rug"],
        },
        "Motion": {
            displayName: "Motion",
            enumeration: ["Impact", "Drop", "Roll", "Slide"],
        },
        "Speed": { displayName: "Speed" },
        "Impact": { displayName: "Impact" },
        "Level": { displayName: "Level" },
    },
    // deck context menu: the physics spans ~75 dB (impacts ~130 dB SPL,
    // rolling / sliding 50-75 dB), so continuous motions start from a
    // raised Level
    defaultConfigurations: [
        { name: "Impact", parameterValues: { "Motion": 0, "Level": 0 } },
        { name: "Drop", parameterValues: { "Motion": 1, "Level": 0 } },
        { name: "Roll", parameterValues: { "Motion": 2, "Level": 35 } },
        { name: "Slide", parameterValues: { "Motion": 3, "Level": 50 } },
    ],
    deckUi: {
        deckWidgetType: studio.ui.deckWidgetType.Layout,
        layout: studio.ui.layoutType.HBoxLayout,
        spacing: 12,
        items: [
            {
                deckWidgetType: studio.ui.deckWidgetType.Layout,
                layout: studio.ui.layoutType.VBoxLayout,
                spacing: 6,
                items: [
                    { deckWidgetType: studio.ui.deckWidgetType.Dropdown, binding: "Object",
                      minimumWidth: 150 },
                    { deckWidgetType: studio.ui.deckWidgetType.Dropdown, binding: "Surface",
                      minimumWidth: 150 },
                    { deckWidgetType: studio.ui.deckWidgetType.Dropdown, binding: "Motion",
                      minimumWidth: 150 },
                ],
            },
            { deckWidgetType: studio.ui.deckWidgetType.Dial, binding: "Speed", color: "#3a7bd5" },
            { deckWidgetType: studio.ui.deckWidgetType.Dial, binding: "Impact", color: "#d5793a" },
            { deckWidgetType: studio.ui.deckWidgetType.Dial, binding: "Level" },
        ],
    },
});
