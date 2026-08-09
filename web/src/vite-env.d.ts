/// <reference types="vite/client" />

interface ImportMetaEnv {
  readonly VITE_PLYWISE_API_ORIGIN?: string;
  readonly VITE_PLYWISE_EVENT_ORIGIN?: string;
  readonly VITE_SUPABASE_URL?: string;
  readonly VITE_SUPABASE_PUBLISHABLE_KEY?: string;
}

interface ImportMeta {
  readonly env: ImportMetaEnv;
}
