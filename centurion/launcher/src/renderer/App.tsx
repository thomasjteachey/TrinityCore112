import { api } from './utils/api';
import WoWLogo from './assets/logo.png';
import LaunchPanel from './components/LaunchPanel';
import TopBar from './components/TopBar';
import IconSpinner from './components/styled/IconSpinner';
import RealmSwitch from './components/RealmSwitch';
import OptionalPatches from './components/OptionalPatches';

const App = () => {
	const { isLoading } = api.preferences.get.useQuery();
	const version = api.general.version.useQuery();

	return (
		<div className="relative flex grow flex-col gap-3 overflow-hidden border border-textDark/10 bg-dark bg-cover bg-top bg-no-repeat p-[44px]">
			<TopBar />

			{isLoading ? (
				<div className="flex flex-grow items-center justify-center">
					<IconSpinner />
				</div>
			) : (
				<>
					<div className="flex select-none items-center gap-4 self-start">
						<img src={WoWLogo} alt="Logo" className="shrink-0" />
                                                <h1 className="uppercase">
                                                        <p className="text-2xl">Centurion</p>
                                                </h1>
					</div>

					<div className="flex min-h-0 flex-grow select-none flex-col items-center gap-2">
						<RealmSwitch />
						<OptionalPatches />
					</div>
					<LaunchPanel />
				</>
			)}

			{!version.isLoading && (
				<p className="absolute bottom-2 right-2 text-xs text-textDark">
					v{version.data}
				</p>
			)}
		</div>
	);
};

export default App;
