# -*- coding:utf-8 -*-
# SPDX-FileCopyrightText: 2025 SiFli
# SPDX-License-Identifier: Apache-2.0

import subprocess
from typing import Any
from typing import Dict
from typing import Optional
from typing import Tuple

from click.core import Context

from sdk_py_actions.sf_pkg_auth import SfPkgAuthError
from sdk_py_actions.sf_pkg_auth import clear_users
from sdk_py_actions.sf_pkg_auth import delete_user
from sdk_py_actions.sf_pkg_auth import get_active_user
from sdk_py_actions.sf_pkg_auth import list_users
from sdk_py_actions.sf_pkg_auth import normalize_user
from sdk_py_actions.sf_pkg_auth import resolve_credentials
from sdk_py_actions.sf_pkg_auth import set_active_user
from sdk_py_actions.sf_pkg_auth import upsert_user
from sdk_py_actions.tools import PropertyDict


def _extract_namespace(package_ref: str) -> Optional[str]:
    if '@' not in package_ref:
        return None

    namespace = package_ref.rsplit('@', 1)[1].strip()
    if not namespace:
        return None

    # Keep compatibility if channel is present in form @user/channel.
    namespace = namespace.split('/', 1)[0].strip()
    if not namespace:
        return None

    return normalize_user(namespace)


def action_extensions(base_actions: Dict, project_path: str) -> Any:

    def get_global_user(args: PropertyDict) -> Optional[str]:
        global_user = args.get('sf_pkg_user')
        if not isinstance(global_user, str):
            return None

        global_user = global_user.strip()
        return global_user if global_user else None

    def resolve_login_user(args: PropertyDict, action_user: Optional[str]) -> str:
        global_user = get_global_user(args)

        if action_user and global_user and normalize_user(action_user) != normalize_user(global_user):
            raise SfPkgAuthError(
                'Conflicting users provided. Please use either global "--user" or '
                '"sf-pkg-login --user", or keep them identical.'
            )

        selected_user = action_user or global_user
        if not selected_user:
            raise SfPkgAuthError(
                'Missing user for login. Use "sdk.py --user <user> sf-pkg-login -t <token>" '
                'or "sdk.py sf-pkg-login -u <user> -t <token>".'
            )

        return normalize_user(selected_user)

    def login_remote(user: str, token: str) -> None:
        subprocess.run([
            'conan', 'remote', 'login', '-p', token, 'artifactory', user,
        ], check=True)

    def ensure_remote_login(args: PropertyDict, required: bool) -> Optional[Tuple[str, str]]:
        credentials = resolve_credentials(get_global_user(args), required=required)
        if credentials is None:
            return None

        user, token = credentials
        login_remote(user, token)
        return user, token

    def init_callback(target_name: str, ctx: Context, args: PropertyDict) -> None:
        try:
            result = subprocess.run(['conan', 'new', 'sf-pkg-project'], check=False)
            if result.returncode == 0:
                print('You can now add dependent packages in conanfile.txt')
            else:
                print('Error: Failed to create dependency file')
        except Exception as e:
            print(f'Error creating dependency file: {e}')

    def install_callback(target_name: str, ctx: Context, args: PropertyDict) -> None:
        try:
            ensure_remote_login(args, required=False)
            subprocess.run([
                'conan', 'install', '.',
                '--output-folder=sf-pkgs',
                '--deployer=full_deploy',
                '--envs-generation=false',
                '-r=artifactory',
            ], check=True)
            print('Packages installed successfully')
        except SfPkgAuthError as e:
            print(f'Error: {e}')
        except subprocess.CalledProcessError as e:
            print(f'Error installing packages: {e}')
        except Exception as e:
            print(f'Error: {e}')

    def new_callback(
        target_name: str,
        ctx: Context,
        args: PropertyDict,
        name: str = 'mypackage',
        version: Optional[str] = None,
        license: Optional[str] = None,
        author: Optional[str] = None,
        support_sdk_version: Optional[str] = None,
    ) -> None:
        try:
            credentials = ensure_remote_login(args, required=True)
            if not credentials:
                print('Error: No available user credentials. Please login first.')
                return

            user, _token = credentials

            cmd = [
                'conan', 'new', 'sf-pkg-package',
                '-d', f'name={name}',
                '-d', f'user={user}',
            ]

            if version:
                cmd.extend(['-d', f'version={version}'])
            if license:
                cmd.extend(['-d', f'license={license}'])
            if author:
                cmd.extend(['-d', f'author={author}'])
            if support_sdk_version:
                cmd.extend(['-d', f'support_sdk_version={support_sdk_version}'])

            subprocess.run(cmd, check=True)
            print(f'Created new package: {name}')
        except SfPkgAuthError as e:
            print(f'Error: {e}')
        except subprocess.CalledProcessError as e:
            print(f'Error creating new package: {e}')
        except Exception as e:
            print(f'Error: {e}')

    def build_callback(target_name: str, ctx: Context, args: PropertyDict, version: str) -> None:
        try:
            ensure_remote_login(args, required=False)
            subprocess.run([
                'conan', 'create',
                '--version', version, '.',
            ], check=True)
            print(f'Package built successfully with version: {version}')
        except SfPkgAuthError as e:
            print(f'Error: {e}')
        except subprocess.CalledProcessError as e:
            print(f'Error building package: {e}')
        except Exception as e:
            print(f'Error: {e}')

    def search_callback(target_name: str, ctx: Context, args: PropertyDict, name: str) -> None:
        try:
            ensure_remote_login(args, required=False)
            search_pattern = name if '*' in name else f'{name}/*'
            subprocess.run(['conan', 'search', search_pattern, '-r=artifactory'], check=True)
            print(f'Search completed for: {search_pattern}')
        except SfPkgAuthError as e:
            print(f'Error: {e}')
        except subprocess.CalledProcessError as e:
            print(f'Error searching for package: {e}')
        except Exception as e:
            print(f'Error: {e}')

    def upload_callback(target_name: str, ctx: Context, args: PropertyDict, name: str, keep: bool = False) -> None:
        try:
            credentials = resolve_credentials(get_global_user(args), required=True)
            if not credentials:
                print('Error: No available user credentials. Please login first.')
                return

            user, token = credentials
            package_user = _extract_namespace(name)
            if package_user and package_user != user:
                print(
                    f'Error: Package namespace "{package_user}" does not match selected user "{user}". '
                    'Use matching --user or update --name.'
                )
                return

            login_remote(user, token)

            subprocess.run(['conan', 'upload', name, '-r=artifactory'], check=True)
            print(f'Package {name} uploaded successfully')

            if not keep:
                subprocess.run(['conan', 'remove', name, '-c'], check=False)
                print(f'Package {name} removed from local cache')

            try:
                import requests

                sync_url = 'https://packages.sifli.com/api/v1/sync'
                headers = {
                    'Authorization': f'Bearer {token}',
                    'Content-Type': 'application/json',
                }

                print('Syncing package to public repository...')
                response = requests.post(sync_url, json={}, headers=headers, timeout=30)

                if response.status_code == 200:
                    print('Package synced to public repository successfully')
                else:
                    print(f'Warning: Sync failed with status code {response.status_code}')
                    print(f'Response: {response.text}')

            except ImportError:
                print('Warning: requests library not available. Skipping sync to public repository.')
            except Exception as e:
                print(f'Warning: Failed to sync to public repository: {e}')

        except SfPkgAuthError as e:
            print(f'Error: {e}')
        except subprocess.CalledProcessError as e:
            print(f'Error uploading package: {e}')
        except Exception as e:
            print(f'Error: {e}')

    def remove_callback(target_name: str, ctx: Context, args: PropertyDict, name: str) -> None:
        try:
            subprocess.run(['conan', 'remove', name, '-c'], check=True)
            print(f'Package {name} removed from local cache')
        except subprocess.CalledProcessError as e:
            print(f'Error removing package: {e}')
        except Exception as e:
            print(f'Error: {e}')

    def login_callback(target_name: str, ctx: Context, args: PropertyDict, token: str, user: Optional[str] = None) -> None:
        """Login to SiFli package registry and store credentials"""
        try:
            selected_user = resolve_login_user(args, user)
            login_remote(selected_user, token)
            print('Logged in to SiFli package registry')

            stored_user = upsert_user(selected_user, token)
            print(f'Credentials stored for user: {stored_user}')
            print(f'Active user set to: {stored_user}')

        except SfPkgAuthError as e:
            print(f'Error: {e}')
        except subprocess.CalledProcessError as e:
            print(f'Error logging in: {e}')
        except Exception as e:
            print(f'Error: {e}')

    def logout_callback(target_name: str, ctx: Context, args: PropertyDict, name: Optional[str] = None) -> None:
        """Logout from SiFli package registry and clear credentials"""
        try:
            target_user = name or get_global_user(args)
            if target_user:
                selected_user = normalize_user(target_user)
                removed = delete_user(selected_user)
                if removed:
                    subprocess.run(['conan', 'remote', 'logout', 'artifactory'], check=False)
                    print(f'Logged out user: {selected_user}')
                    current = get_active_user()
                    if current:
                        print(f'Active user switched to: {current}')
                else:
                    print(f'User {selected_user} not found in stored credentials')
            else:
                subprocess.run(['conan', 'remote', 'logout', 'artifactory'], check=False)
                clear_users()
                print('Logged out all users and cleared credentials')

        except subprocess.CalledProcessError as e:
            print(f'Error logging out: {e}')
        except Exception as e:
            print(f'Error: {e}')

    def users_callback(target_name: str, ctx: Context, args: PropertyDict) -> None:
        try:
            users = list_users()
            active = get_active_user()
            if not users:
                print('No stored sf-pkg users. Please login using "sdk.py sf-pkg-login".')
                return

            print('Stored sf-pkg users:')
            for user in users:
                marker = '*' if user == active else ' '
                print(f' {marker} {user}')
            if active:
                print(f'Active user: {active}')
        except Exception as e:
            print(f'Error: {e}')

    def use_callback(target_name: str, ctx: Context, args: PropertyDict, name: str) -> None:
        try:
            selected_user = set_active_user(name)
            print(f'Active sf-pkg user set to: {selected_user}')
        except SfPkgAuthError as e:
            print(f'Error: {e}')
        except Exception as e:
            print(f'Error: {e}')

    def current_user_callback(target_name: str, ctx: Context, args: PropertyDict) -> None:
        try:
            active = get_active_user()
            if not active:
                print('No active sf-pkg user. Use "sdk.py sf-pkg-login" or "sdk.py sf-pkg-use" first.')
                return
            print(active)
        except Exception as e:
            print(f'Error: {e}')

    sf_pkg_actions = {
        'global_options': [
            {
                'names': ['--user', '-u', 'sf_pkg_user'],
                'help': 'Select sf-pkg user globally, e.g. "sdk.py --user <name> sf-pkg-upload ...".',
                'default': None,
            },
        ],
        'actions': {
            'sf-pkg-login': {
                'callback': login_callback,
                'help': 'Login to SiFli package registry and store credentials.',
                'options': [
                    {
                        'names': ['--user', '-u'],
                        'help': 'Username for login. If omitted, global --user is used.',
                        'required': False,
                        'default': None,
                    },
                    {
                        'names': ['--token', '-t'],
                        'help': 'API token for login.',
                        'required': True,
                    },
                ],
            },
            'sf-pkg-logout': {
                'callback': logout_callback,
                'help': 'Logout from SiFli package registry and clear credentials.',
                'options': [
                    {
                        'names': ['--name', '-n'],
                        'help': 'Username to logout (if not specified, logout selected user by global --user, or all users).',
                        'required': False,
                        'default': None,
                    },
                ],
            },
            'sf-pkg-users': {
                'callback': users_callback,
                'help': 'List local sf-pkg users and active user.',
            },
            'sf-pkg-use': {
                'callback': use_callback,
                'help': 'Switch active sf-pkg user.',
                'options': [
                    {
                        'names': ['--name', '-n'],
                        'help': 'Username to set as active user.',
                        'required': True,
                    },
                ],
            },
            'sf-pkg-current-user': {
                'callback': current_user_callback,
                'help': 'Show current active sf-pkg user.',
            },
            'sf-pkg-init': {
                'callback': init_callback,
                'help': 'Initialize project dependencies.',
            },
            'sf-pkg-install': {
                'callback': install_callback,
                'help': 'Install SiFli-SDK packages.',
            },
            'sf-pkg-new': {
                'callback': new_callback,
                'help': 'Create a new SiFli-SDK package.',
                'options': [
                    {
                        'names': ['--name', '-n'],
                        'help': 'Package name.',
                        'default': 'mypackage',
                        'required': True,
                    },
                    {
                        'names': ['--version'],
                        'help': 'Package version.',
                        'required': False,
                        'default': None,
                    },
                    {
                        'names': ['--license'],
                        'help': 'Package license.',
                        'required': False,
                        'default': None,
                    },
                    {
                        'names': ['--author'],
                        'help': 'Package author.',
                        'required': False,
                        'default': None,
                    },
                    {
                        'names': ['--support-sdk-version'],
                        'help': 'Supported SiFli-SDK version.',
                        'required': False,
                        'default': None,
                    },
                ],
            },
            'sf-pkg-build': {
                'callback': build_callback,
                'help': 'Build the package for upload.',
                'options': [
                    {
                        'names': ['--version', '-v'],
                        'help': 'Version to be built.',
                        'required': True,
                    },
                ],
            },
            'sf-pkg-upload': {
                'callback': upload_callback,
                'help': 'Upload the specified package.',
                'options': [
                    {
                        'names': ['--name', '-n'],
                        'help': 'Name of package to be uploaded.',
                        'required': True,
                    },
                    {
                        'names': ['--keep'],
                        'help': 'Keep local cache after upload.',
                        'is_flag': True,
                        'default': False,
                    },
                ],
            },
            'sf-pkg-remove': {
                'callback': remove_callback,
                'help': 'Remove package from local cache.',
                'options': [
                    {
                        'names': ['--name'],
                        'help': 'Name of package to be removed.',
                        'required': True,
                    },
                ],
            },
            'sf-pkg-search': {
                'callback': search_callback,
                'help': 'Search for packages in SiFli package registry.',
                'arguments': [
                    {
                        'names': ['name'],
                        'required': True,
                    },
                ],
            },
        },
    }

    return sf_pkg_actions
