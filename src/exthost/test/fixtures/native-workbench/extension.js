'use strict';

const vscode = require('vscode');

exports.activate = (context) => {
  const command = vscode.commands.registerCommand('sakuraTest.native.run', () => 'native-command-ok');

  const status = vscode.window.createStatusBarItem(
    'sakuraTest.native.status', vscode.StatusBarAlignment.Left, 100,
  );
  status.text = '$(extensions) VSX Ready';
  status.tooltip = 'Native workbench bridge is connected';
  status.command = 'sakuraTest.native.run';
  status.accessibilityInformation = { label: 'Open VSX compatibility ready' };
  status.show();

  const root = { label: 'Open VSX tree item' };
  const provider = {
    getChildren(element) {
      return element ? [] : [root];
    },
    getTreeItem(element) {
      const item = new vscode.TreeItem(element.label, vscode.TreeItemCollapsibleState.None);
      item.id = 'open-vsx-root';
      item.description = 'Native sidebar';
      item.tooltip = 'Provided by a VS Code-compatible extension';
      item.command = { command: 'sakuraTest.native.run', title: 'Run' };
      return item;
    },
  };
  const view = vscode.window.createTreeView('sakuraTest.native.view', {
    treeDataProvider: provider,
    title: 'Open VSX Test View',
    showCollapseAll: true,
  });
  view.description = 'Connected';

  const formatter = vscode.languages.registerDocumentFormattingEditProvider('plaintext', {
    provideDocumentFormattingEdits(document) {
      const wholeDocument = new vscode.Range(
        new vscode.Position(0, 0),
        document.positionAt(document.getText().length),
      );
      return [vscode.TextEdit.replace(wholeDocument, document.getText().toUpperCase())];
    },
  });

  context.subscriptions.push(command, status, view, formatter);
};
