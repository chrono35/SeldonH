/**************************************************************************
**
** SeldonH - controlscript.qs  (repris de ChronoModel, version du 2026-10-16)
**
** Embarqué dans l'installeur ET dans le désinstalleur (SeldonH_uninstaller).
**
** 1. Programme lancé en fin d'installation selon l'OS. Linux : installation
**    dans ~/SeldonH (pas de droits root), lancement par SeldonH.sh
**    (il positionne LD_LIBRARY_PATH). macOS : /Applications/SeldonH/SeldonH.app.
** 2. Correctif du bug de la traduction française de QtIFW 4.10/4.11
**    (ifw_fr.qm) : le message de la page « Résumé de l'installation » y est
**    stocké avec le HTML déjà échappé ; on décode les entités et on force
**    l'affichage en texte riche.
**
**************************************************************************/

function Controller()
{
    if (systemInfo.kernelType === "linux") {
        if (installer.isInstaller())
            installer.setValue("TargetDir", installer.value("HomeDir") + "/SeldonH");
        installer.setValue("RunProgram", "@TargetDir@/SeldonH.sh");
    }
    else if (systemInfo.kernelType === "darwin") {
        installer.setValue("RunProgram", "@TargetDir@/SeldonH.app/Contents/MacOS/SeldonH");
    }
    else if (systemInfo.productType === "windows") {
        installer.setValue("RunProgram", "@TargetDir@/SeldonH.exe");
    }
}

function decodeHtmlEntities(s)
{
    return s.replace(/&lt;/g, "<")
            .replace(/&gt;/g, ">")
            .replace(/&quot;/g, "\"")
            .replace(/&#39;/g, "'")
            .replace(/&amp;/g, "&");   // en dernier
}

Controller.prototype.ReadyForInstallationPageCallback = function()
{
    var page = gui.currentPageWidget();
    if (page === null || page.MessageLabel === undefined)
        return;

    var label = page.MessageLabel;
    var txt = label.text;
    if (txt.indexOf("&lt;") >= 0)
        txt = decodeHtmlEntities(txt);

    label.textFormat = 1;   // Qt::RichText
    label.text = txt;
}
