import * as vscode from "vscode";

const LEI_KEYWORDS = [
  "import",
  "from",
  "export",
  "proto",
  "plan",
  "let",
  "var",
  "def",
  "assert",
  "if",
  "else",
  "true",
  "false",
  "int",
  "float",
  "string",
  "bool",
  "return",
  "for",
  "in",
] as const;

interface LeiSnippetSpec {
  label: string;
  detail: string;
  body: string;
}

const LEI_SNIPPETS: LeiSnippetSpec[] = [
  {
    label: "import/from",
    detail: "LEI import alias snippet",
    body: 'import ${1:alias} from "${2:./module.lei}";',
  },
  {
    label: "proto",
    detail: "LEI proto declaration snippet",
    body: "proto ${1:BuildConfig} {\n\t${2:name}: string;\n\t${3:jobs}: int = ${4:8};\n}",
  },
  {
    label: "plan",
    detail: "LEI plan declaration snippet",
    body: "plan ${1:master} {\n\t${2:key} = ${3:value};\n};",
  },
  {
    label: "def",
    detail: "LEI function declaration snippet",
    body: "def ${1:name}(${2:param}: ${3:string}) -> ${4:bool} {\n\t${5:return true;}\n}",
  },
  {
    label: "for",
    detail: "LEI for-in loop snippet",
    body: "for ${1:item} in ${2:items} {\n\t${3:// todo}\n}",
  },
  {
    label: "if/else",
    detail: "LEI if/else snippet",
    body: "if (${1:cond}) {\n\t${2:// then}\n} else {\n\t${3:// else}\n}",
  },
  {
    label: "assert",
    detail: "LEI assert snippet",
    body: "assert ${1:expr};",
  },
  {
    label: "export plan",
    detail: "LEI export plan snippet",
    body: "export plan ${1:release};",
  },
];

export function registerLeiCompletionProvider(): vscode.Disposable {
  return vscode.languages.registerCompletionItemProvider(
    [{ language: "lei", scheme: "file" }],
    {
      provideCompletionItems() {
        const items: vscode.CompletionItem[] = [];

        for (const keyword of LEI_KEYWORDS) {
          const item = new vscode.CompletionItem(keyword, vscode.CompletionItemKind.Keyword);
          item.detail = "LEI keyword";
          items.push(item);
        }

        for (const spec of LEI_SNIPPETS) {
          const item = new vscode.CompletionItem(spec.label, vscode.CompletionItemKind.Snippet);
          item.detail = spec.detail;
          item.insertText = new vscode.SnippetString(spec.body);
          items.push(item);
        }

        return items;
      },
    }
  );
}
