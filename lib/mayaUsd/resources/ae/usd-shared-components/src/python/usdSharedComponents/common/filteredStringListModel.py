from typing import Sequence

try:
    from PySide6.QtCore import (
        QStringListModel,
        Signal,
    )
except:
    from PySide2.QtCore import (
        QStringListModel,
        Signal,
    )


class FilteredStringListModel(QStringListModel):
    '''
    A Qt string list model that can be filtered.
    '''
    filterChanged = Signal()

    def __init__(self, items: Sequence[str] = None, parent=None):
        super(FilteredStringListModel, self).__init__(items if items else [], parent)
        self._unfilteredItems = items
        self._isFilteredEmpty = False
        self._filter = ""

    def setStringList(self, items: Sequence[str]):
        '''
        Override base class implementation to properly rebuild
        the filtered list.
        '''
        self._unfilteredItems = items
        self._isFilteredEmpty = False
        super(FilteredStringListModel, self).setStringList(items)
        self._rebuildFilteredModel()

    def _rebuildFilteredModel(self):
        '''
        Rebuild the model by applying the filter.
        '''
        if not self._filter:
            filteredItems = self._unfilteredItems
        else:
            filters = [filter.lower() for filter in self._filter.split('*')]
            filteredItems = [item for item in self._unfilteredItems if self._isValidItem(item, filters)]
        self._isFilteredEmpty = bool(self._unfilteredItems) and not bool(filteredItems)
        # Note: don't call our own version, otehrwise we would get infinite recursion.
        super(FilteredStringListModel, self).setStringList(filteredItems)

    def _isValidItem(self, item: str, filters: Sequence[str]):
        '''
        Verify if the item passes all the filters.

        We search each given filter in sequence, each one must match
        somewhere in the item starting at the end of the point where
        the preceeding filter ended:
        '''
        index = 0
        item = item.lower()
        for filter in filters:
            newIndex = item.find(filter, index)
            if newIndex < 0:
                return False
            index = newIndex + len(filter)
        return True
    
    def isFilteredEmpty(self):
        '''
        Verify if the model is empty because it was entirely filtered out.
        '''
        return self._isFilteredEmpty

    def setFilter(self, filter: str):
        '''
        Set the filter to be applied to the model and rebuild the model.
        '''
        if filter == self._filter:
            return
        self._filter = filter
        self._rebuildFilteredModel()
        self.filterChanged.emit()