/**************************************************************************
** SeldonH - installscript.qs (composant seldonh.app)
** Raccourcis : menu Démarrer + Bureau sous Windows, entrée de menu sous Linux.
**************************************************************************/

function Component()
{
}

Component.prototype.createOperations = function()
{
    component.createOperations();   // copie des fichiers

    if (systemInfo.productType === "windows") {
        component.addOperation("CreateShortcut",
            "@TargetDir@/SeldonH.exe", "@StartMenuDir@/SeldonH.lnk",
            "workingDirectory=@TargetDir@", "iconPath=@TargetDir@/SeldonH.exe", "iconId=0",
            "description=Assistant de datation pour ChronoModel");
        component.addOperation("CreateShortcut",
            "@TargetDir@/SeldonH.exe", "@DesktopDir@/SeldonH.lnk",
            "workingDirectory=@TargetDir@", "iconPath=@TargetDir@/SeldonH.exe", "iconId=0");
        component.addOperation("CreateShortcut",
            "@TargetDir@/SeldonH_uninstaller.exe", "@StartMenuDir@/Désinstaller SeldonH.lnk",
            "workingDirectory=@TargetDir@");
    }
    else if (systemInfo.kernelType === "linux") {
        component.addOperation("CreateDesktopEntry",
            "@HomeDir@/.local/share/applications/SeldonH.desktop",
            "Type=Application\nName=SeldonH\nComment=Assistant de datation pour ChronoModel\n"
            + "Exec=@TargetDir@/SeldonH.sh %F\nIcon=@TargetDir@/seldonh.png\nTerminal=false\n"
            + "Categories=Science;Education;");
    }
}
