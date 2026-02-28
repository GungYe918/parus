import {
  LanguageClient,
  type LanguageClientOptions,
  type ServerOptions,
} from "vscode-languageclient/node";

export function createParusLanguageClient(
  serverOptions: ServerOptions,
  clientOptions: LanguageClientOptions
): LanguageClient {
  return new LanguageClient("parus-language-server", "Parus Language Server", serverOptions, clientOptions);
}
