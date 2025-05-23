from ..common.host import Host
from ..common.theme import Theme

try:
    from PySide6.QtWidgets import QToolButton # type: ignore
except ImportError:
    from PySide2.QtWidgets import QToolButton # type: ignore


CONFLICT_WARNING_TOOLTIP = "Both Include/Exclude rules and Expressions are currently defined.\nExpressions will be ignored."


class WarningWidget(QToolButton):
    '''
    Widget to show a warning symbol and tooltip when collection data
    has a conflict between expression and include/exclude attributes.
    '''

    def __init__(self, parentWidget=None):
        super(WarningWidget, self).__init__(parentWidget)
        self._conflicted = False

        theme = Theme.instance()
        
        self.setToolTip(theme.themeLabel(CONFLICT_WARNING_TOOLTIP))
        self.setStatusTip(theme.themeLabel(CONFLICT_WARNING_TOOLTIP))
        self.setIcon(theme.icon("warning"))
        self.setVisible(False)
        self.setStyleSheet("""
            QToolButton { border: 0px; }""")

    def isConflicted(self) -> bool:
        return self._conflicted
    
    def setConflicted(self, isConflicted: bool):
        self._conflicted = isConflicted
        self.setVisible(isConflicted)
