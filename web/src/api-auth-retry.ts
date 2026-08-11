export type AuthRetryResponse = { readonly status: number };

/**
 * Retries one authenticated request after Supabase returns an expired/invalid
 * bearer response. A missing or unchanged token never triggers a retry.
 */
export async function retryWithFreshAuth<T extends AuthRetryResponse>(
  initialToken: string | null,
  request: (token: string | null) => Promise<T>,
  refresh: () => Promise<string | null>,
): Promise<T> {
  const response = await request(initialToken);
  if (response.status !== 401 || !initialToken) return response;
  const refreshedToken = await refresh();
  if (!refreshedToken || refreshedToken === initialToken) return response;
  return request(refreshedToken);
}
