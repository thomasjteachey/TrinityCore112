import { StrictMode } from 'react';
import ReactDOM from 'react-dom/client';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { loggerLink } from '@trpc/client';
import { ipcLink } from 'electron-trpc/renderer';
import superjson from 'superjson';
import { ReactQueryDevtools } from '@tanstack/react-query-devtools';
import { getQueryKey } from '@trpc/react-query';

import { api } from './utils/api';
import App from './App';

import './index.css';

export const queryClient = new QueryClient({
	defaultOptions: {
		queries: {
			refetchOnWindowFocus: false,
			staleTime: Infinity
		}
	}
});

queryClient.setMutationDefaults(getQueryKey(api.preferences.set), {
	onSuccess: v =>
		queryClient.setQueryData(
			getQueryKey(api.preferences.get, undefined, 'query'),
			v
		)
});

const trpcClient = api.createClient({
	/**
	 * Transformer used for data de-serialization from the server.
	 *
	 * @see https://trpc.io/docs/data-transformers
	 */
	transformer: superjson,

	/**
	 * Links used to determine request flow from client to server.
	 *
	 * @see https://trpc.io/docs/links
	 */
	links: [
		loggerLink({
			enabled: opts =>
				process.env.NODE_ENV === 'development' ||
				(opts.direction === 'down' && opts.result instanceof Error)
		}),
		ipcLink()
	]
});

ReactDOM.createRoot(document.getElementById('root') as HTMLElement).render(
	<StrictMode>
		<api.Provider client={trpcClient} queryClient={queryClient}>
			<QueryClientProvider client={queryClient}>
				<App />
				<ReactQueryDevtools />
			</QueryClientProvider>
		</api.Provider>
	</StrictMode>
);
