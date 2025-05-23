

//
// Copyright 2020 Autodesk
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "mayaLayerEditorWindow.h"

#include "layerEditorWidget.h"
#include "layerTreeModel.h"
#include "layerTreeView.h"
#include "mayaQtUtils.h"
#include "mayaSessionState.h"
#include "sessionState.h"

#include <mayaUsd/utils/query.h>

#include <maya/MGlobal.h>
#include <maya/MQtUtil.h>

#include <QtCore/QPointer>

#include <map>
#include <vector>

namespace {
using namespace UsdLayerEditor;

class LayerEditorWindowCreator : public MayaUsd::AbstractLayerEditorCreator
{
public:
    LayerEditorWindowCreator() { ; };
    virtual ~LayerEditorWindowCreator() { }

    MayaUsd::AbstractLayerEditorWindow* createWindow(const char* panelName) override;
    MayaUsd::AbstractLayerEditorWindow* getWindow(const char* panelName) const override;
    std::vector<std::string>            getAllPanelNames() const override;

private:
    // it's very important that this be a QPointer, so that it gets
    // automatically nulled if the window gets closed
    typedef std::map<std::string, QPointer<MayaLayerEditorWindow>> EditorsMap;

    static EditorsMap _editors;
} g_layerEditorWindowCreator;

LayerEditorWindowCreator::EditorsMap LayerEditorWindowCreator::_editors;

MayaUsd::AbstractLayerEditorWindow* LayerEditorWindowCreator::createWindow(const char* panelName)
{
    auto _workspaceControl = MQtUtil::getCurrentParent();
    auto editorWindow = new MayaLayerEditorWindow(panelName, nullptr);
    _editors[panelName] = editorWindow;

    // Add UI as a child of the workspace control
    MQtUtil::addWidgetToMayaLayout(editorWindow, _workspaceControl);
    return editorWindow;
}

MayaUsd::AbstractLayerEditorWindow* LayerEditorWindowCreator::getWindow(const char* panelName) const
{
    auto window = _editors.find(panelName);
    return (window == _editors.end()) ? nullptr : window->second;
}

MayaUsd::AbstractLayerEditorCreator::PanelNamesList
LayerEditorWindowCreator::getAllPanelNames() const
{
    PanelNamesList result;
    for (auto entry : _editors) {
        result.push_back(entry.first);
    }
    return result;
}

} // namespace

namespace UsdLayerEditor {

MayaLayerEditorWindow::MayaLayerEditorWindow(const char* panelName, QWidget* parent)
    : PARENT_CLASS(parent)
    , MayaUsd::AbstractLayerEditorWindow(panelName)
    , _panelName(panelName)
{
    // Normally this will be set from the MayaUsd plugin, but only
    // when building with UFE (for the batch save case).
    if (!UsdLayerEditor::utils) {
        UsdLayerEditor::utils = new MayaQtUtils();
    }
    onCreateUI();

    connect(
        &_sessionState,
        &MayaSessionState::clearUIOnSceneResetSignal,
        this,
        &MayaLayerEditorWindow::onClearUIOnSceneReset);
}

MayaLayerEditorWindow::~MayaLayerEditorWindow() { }

void MayaLayerEditorWindow::onClearUIOnSceneReset()
{
    // I'm not sure if I need this, but in earlier prototypes it was
    // safer to delete the entire UI and re-recreate it on scene changes
    // to release all the proxies
    LayerTreeModel::suspendUsdNotices(true);
    setCentralWidget(nullptr);
    delete _layerEditor;
    _sessionState.setStageEntry(UsdLayerEditor::SessionState::StageEntry {});

    QTimer::singleShot(0, this, &MayaLayerEditorWindow::onCreateUI);
}

void MayaLayerEditorWindow::onCreateUI()
{
    LayerTreeModel::suspendUsdNotices(false);
    _layerEditor = new LayerEditorWidget(_sessionState, this);
    setCentralWidget(_layerEditor);
    _layerEditor->show();

    connect(
        treeView(),
        &QWidget::customContextMenuRequested,
        this,
        &MayaLayerEditorWindow::onShowContextMenu);
}

LayerTreeView* MayaLayerEditorWindow::treeView() { return _layerEditor->layerTree(); }

int MayaLayerEditorWindow::selectionLength()
{
    auto selection = treeView()->getSelectedLayerItems();
    return static_cast<int>(selection.size());
}

bool MayaLayerEditorWindow::hasCurrentLayerItem()
{
    auto item = treeView()->currentLayerItem();
    return (item != nullptr);
}

#define CALL_CURRENT_ITEM(method)               \
    auto item = treeView()->currentLayerItem(); \
    return (item == nullptr) ? false : item->method()

bool MayaLayerEditorWindow::isInvalidLayer() { CALL_CURRENT_ITEM(isInvalidLayer); }
bool MayaLayerEditorWindow::isSessionLayer() { CALL_CURRENT_ITEM(isSessionLayer); }
bool MayaLayerEditorWindow::isLayerDirty() { CALL_CURRENT_ITEM(isDirty); }
bool MayaLayerEditorWindow::isSubLayer() { CALL_CURRENT_ITEM(isSublayer); }
bool MayaLayerEditorWindow::isAnonymousLayer() { CALL_CURRENT_ITEM(isAnonymous); }
bool MayaLayerEditorWindow::isIncomingLayer() { CALL_CURRENT_ITEM(isIncoming); }
bool MayaLayerEditorWindow::layerNeedsSaving() { CALL_CURRENT_ITEM(needsSaving); }
bool MayaLayerEditorWindow::layerAppearsMuted() { CALL_CURRENT_ITEM(appearsMuted); }
bool MayaLayerEditorWindow::layerIsMuted() { CALL_CURRENT_ITEM(isMuted); }
bool MayaLayerEditorWindow::layerIsReadOnly() { CALL_CURRENT_ITEM(isReadOnly); }
bool MayaLayerEditorWindow::layerAppearsLocked() { CALL_CURRENT_ITEM(appearsLocked); }
bool MayaLayerEditorWindow::layerIsLocked() { CALL_CURRENT_ITEM(isLocked); }
bool MayaLayerEditorWindow::layerAppearsSystemLocked() { CALL_CURRENT_ITEM(appearsSystemLocked); }
bool MayaLayerEditorWindow::layerIsSystemLocked() { CALL_CURRENT_ITEM(isSystemLocked); }
bool MayaLayerEditorWindow::layerHasSubLayers() { CALL_CURRENT_ITEM(hasSubLayers); }

std::string MayaLayerEditorWindow::proxyShapeName(const bool fullPath) const
{
    auto stageEntry = _sessionState.stageEntry();
    return fullPath ? stageEntry._proxyShapePath : stageEntry._displayName;
}

void MayaLayerEditorWindow::removeSubLayer()
{
    QString name = "Remove";
    treeView()->callMethodOnSelectionNoDelay(name, &LayerTreeItem::removeSubLayer);
}

void MayaLayerEditorWindow::saveEdits()
{
    auto item = treeView()->currentLayerItem();
    if (item) {
        QString name = item->isAnonymous() ? "Save As..." : "Save Edits";
        treeView()->callMethodOnSelection(name, &LayerTreeItem::saveEdits);
    }
}

void MayaLayerEditorWindow::discardEdits()
{
    QString name = "Discard Edits";
    treeView()->callMethodOnSelection(name, &LayerTreeItem::discardEdits);
}

void MayaLayerEditorWindow::addAnonymousSublayer()
{
    QString name = "Add Sublayer";
    treeView()->callMethodOnSelection(name, &LayerTreeItem::addAnonymousSublayer);
}

void MayaLayerEditorWindow::updateLayerModel() { _sessionState.refreshCurrentStageEntry(); }

void MayaLayerEditorWindow::lockLayer()
{
    auto item = treeView()->currentLayerItem();
    if (item != nullptr) {
        QString name = item->isLocked() ? "Unlock" : "Lock";
        treeView()->onLockLayer(name);
    }
}

void MayaLayerEditorWindow::lockLayerAndSubLayers()
{
    auto item = treeView()->currentLayerItem();
    if (item != nullptr) {
        QString name = item->isLocked() ? "Unlock Layer and Sublayers" : "Lock Layer and Sublayers";
        bool    includeSubLayers = true;
        treeView()->onLockLayerAndSublayers(name, includeSubLayers);
    }
}

void MayaLayerEditorWindow::addParentLayer()
{
    QString name = "Add Parent Layer";
    treeView()->onAddParentLayer(name);
}

void MayaLayerEditorWindow::loadSubLayers()
{
    auto item = treeView()->currentLayerItem();
    if (item) {
        item->loadSubLayers(this);
    }
}

void MayaLayerEditorWindow::muteLayer()
{
    auto item = treeView()->currentLayerItem();
    if (item != nullptr) {
        QString name = item->isMuted() ? "Unmute" : "Mute";
        treeView()->onMuteLayer(name);
    }
}

void MayaLayerEditorWindow::printLayer()
{
    QString name = "Print to Script Editor";
    treeView()->callMethodOnSelection(name, &LayerTreeItem::printLayer);
}

void MayaLayerEditorWindow::clearLayer()
{
    // Suspend usd notification while clearing and force a refresh after
    // all layers are cleared. This is required because callMethodOnSelection()
    // will loop on all selected layers and clear them one by one, If a refresh
    // happen before callMethodOnSelection() finish to loop over the selected item,
    // maya will crash because of a dangling pointer (All layer item are deleted
    // during the refresh).
    LayerTreeModel::suspendUsdNotices(true);

    QString name = "Clear";
    treeView()->callMethodOnSelection(name, &LayerTreeItem::clearLayer);

    LayerTreeModel::suspendUsdNotices(false);

    LayerTreeModel* model = treeView()->layerTreeModel();
    model->forceRefresh();
}

void MayaLayerEditorWindow::selectPrimsWithSpec()
{
    auto item = treeView()->currentLayerItem();
    if (item != nullptr) {
        _sessionState.commandHook()->selectPrimsWithSpec(item->layer());
    }
}

void MayaLayerEditorWindow::selectProxyShape(const char* shapePath)
{
    SessionState::StageEntry entry;
    if (_sessionState.getStageEntry(&entry, shapePath)) {
        if (entry._stage != nullptr) {
            _sessionState.setStageEntry(entry);
        }
    }
}

void MayaLayerEditorWindow::onShowContextMenu(const QPoint& pos)
{
    QMenu contextMenu;

    MString menuName = "UsdLayerEditorContextMenu";
    contextMenu.setObjectName(menuName.asChar());
    contextMenu.setSeparatorsCollapsible(false); // elimitates a maya gltich with divider

    MString setParent;
    setParent.format("setParent -menu ^1s;", menuName);

    MString command;
    command.format("mayaUsdMenu_layerEditorContextMenu(\"^1s\");", _panelName.c_str());

    MGlobal::executeCommand(
        setParent + command,
        /*display*/ false,
        /*undo*/ false);

    contextMenu.exec(treeView()->mapToGlobal(pos));
}

std::vector<std::string> MayaLayerEditorWindow::getSelectedLayers()
{
    std::vector<std::string> selLayers;
    if (_layerEditor) {
        selLayers = _layerEditor->getSelectedLayers();
    } else {
        TF_CODING_ERROR(
            "No LayerEditorWidget set in the MayaLayerEditorWindow. No layers to retrieve.");
    }

    return selLayers;
}

void MayaLayerEditorWindow::selectLayers(std::vector<std::string> layerIds)
{
    if (_layerEditor) {
        _layerEditor->selectLayers(layerIds);
    } else {
        TF_CODING_ERROR(
            "No LayerEditorWidget set in the MayaLayerEditorWindow. Layers cannot be selected.");
    }
}

} // namespace UsdLayerEditor
