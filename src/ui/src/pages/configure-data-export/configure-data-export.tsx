/*
 * Copyright 2018- The Pixie Authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

import * as React from 'react';

import { gql, useQuery } from '@apollo/client';
import { Button, Typography } from '@mui/material';
import { Theme } from '@mui/material/styles';
import { createStyles, makeStyles } from '@mui/styles';
import { Link, useRouteMatch, Route, Switch } from 'react-router-dom';

import { ClusterContext } from 'app/common/cluster-context';
import { isPixieEmbedded } from 'app/common/embed-context';
import { buildClass, Footer, scrollbarStyles } from 'app/components';
import { usePluginList } from 'app/containers/admin/plugins/plugin-gql';
import { selectClusterName } from 'app/containers/App/cluster-info';
import { LiveRouteContext } from 'app/containers/App/live-routing';
import NavBars from 'app/containers/App/nav-bars';
import { SidebarContext } from 'app/context/sidebar-context';
import { GQLClusterInfo, GQLClusterStatus, GQLPluginKind } from 'app/types/schema';
import { WithChildren } from 'app/utils/react-boilerplate';
import * as pixienautCarryingBoxes from 'assets/images/pixienaut-carrying-boxes.svg';
import { Copyright } from 'configurable/copyright';

import { EditDataExportScript } from './data-export-detail';
import { ConfigureDataExportBody } from './data-export-tables';

const useStyles = makeStyles((theme: Theme) => createStyles({
  root: {
    width: '100%',
    height: '100%',
    display: 'flex',
    flexDirection: 'column',
    ...scrollbarStyles(theme),
  },
  title: {
    flexGrow: 1,
    marginLeft: theme.spacing(2),
    height: '100%',
  },
  titleText: {
    ...theme.typography.h6,
    color: theme.palette.foreground.grey5,
    fontWeight: theme.typography.fontWeightBold,
    display: 'flex',
    alignItems: 'center',
    height: '100%',
  },
  main: {
    marginLeft: theme.spacing(8),
    flex: 1,
    minHeight: 0,
    padding: theme.spacing(1),
    display: 'flex',
    flexFlow: 'column nowrap',
    overflow: 'auto',
  },
  mainEmbedded: {
    marginLeft: 0,
  },
  mainBlock: {
    flex: '1 0 auto',
    position: 'relative',
  },
  mainFooter: {
    flex: '0 0 auto',
  },
  splashBlock: {
    width: '100%',
    height: '100%',
    display: 'flex',
    flexFlow: 'column nowrap',
    justifyContent: 'center',
    alignItems: 'center',
    textAlign: 'center',

    '& > *': {
      maxWidth: theme.breakpoints.values.sm,
      margin: `${theme.spacing(4)} 0`,
    },
  },
}), { name: 'ConfigureDataExportView' });

// Sidebar navigation links for Live scripts need a cluster that's usually in the URL.
// Since this isn't the live view, we need to provide that context manually.
// Only the embed state and the cluster name actually get used; the rest can be empty values.
const ExtraNavContext = React.memo<WithChildren>(({ children }) => {
  const { data } = useQuery<{
    clusters: Pick<GQLClusterInfo, 'clusterName' | 'status'>[]
  }>(
    gql`
      query listClustersForDataExportRouting {
        clusters {
          id
          clusterName
          status
        }
      }
    `,
    { fetchPolicy: 'cache-first' },
  );

  const defaultCluster = React.useMemo(() => selectClusterName(data?.clusters ?? []), [data?.clusters]);

  return (
    // eslint-disable-next-line react-memo/require-usememo
    <LiveRouteContext.Provider value={{
      embedState: { isEmbedded: false, disableTimePicker: false, widget: null },
      clusterName: '',
      scriptId: '',
      args: {},
    }}>
      {/* eslint-disable-next-line react-memo/require-usememo */}
      <ClusterContext.Provider value={{
        loading: false,
        selectedClusterID: '',
        selectedClusterName: defaultCluster ?? '',
        selectedClusterPrettyName: '',
        selectedClusterStatus: GQLClusterStatus.CS_UNKNOWN,
        selectedClusterStatusMessage: '',
        selectedClusterUID: '',
        selectedClusterVizierConfig: null,
        setClusterByName: () => {},
      }}>
        {children}
      </ClusterContext.Provider>
    </LiveRouteContext.Provider>
  );
});
ExtraNavContext.displayName = 'ExtraNavContext';

const ConfigureDataExportPage = React.memo<WithChildren>(({ children }) => {
  const classes = useStyles();
  const isEmbedded = isPixieEmbedded();
  return (
    <div className={classes.root}>
      {!isEmbedded && (
        <ExtraNavContext>
          <SidebarContext.Provider value={{ showLiveOptions: false, showAdmin: true }}>
            <NavBars>
              <div className={classes.title}>
                <div className={classes.titleText}>Long-term Data Export</div>
              </div>
            </NavBars>
          </SidebarContext.Provider>
        </ExtraNavContext>
      )}
      <div className={buildClass(classes.main, isEmbedded && classes.mainEmbedded)}>
        <div className={classes.mainBlock}>
          {children}
        </div>
        <div className={classes.mainFooter}>
          <Footer copyright={Copyright} />
        </div>
      </div>
    </div>
  );
});
ConfigureDataExportPage.displayName = 'ConfigureDataExportPage';

const NoPluginsEnabledSplash = React.memo(() => {
  const classes = useStyles();
  const isEmbedded = isPixieEmbedded();
  return (
    <div className={classes.splashBlock}>
      <img src={pixienautCarryingBoxes} alt='Long-term Data Export Setup' />
      {isEmbedded ? (
        <Typography variant='body2'>
          To use this page, at least one Pixie plugin supporting long-term data export must be enabled.
          <br />
          If you&apos;re seeing this page embedded in another, something has likely been misconfigured.
        </Typography>
      ) : (
        <>
          <Typography variant='body2'>
            Pixie only guarantees data retention for 24 hours.
            <br />
            {'Configure a '}
            <Link to='/admin/plugins'>plugin</Link>
            {' to export and store Pixie data for longer term retention.'}
            <br />
            This data will be accessible and queryable through the plugin provider.
          </Typography>
          <Button component={Link} to='/admin/plugins' variant='contained'>Configure Plugins</Button>
        </>
      )}
    </div>
  );
});
NoPluginsEnabledSplash.displayName = 'NoPluginsEnabledSplash';

export const ConfigureDataExportView = React.memo(() => {
  const { plugins } = usePluginList(GQLPluginKind.PK_RETENTION);
  const { path } = useRouteMatch();

  const isSplash = React.useMemo(() => (
    !plugins.some(p => p.supportsRetention && p.retentionEnabled)
  ), [plugins]);

  return (
    <ConfigureDataExportPage>
      <Switch>
        <Route exact path={path}>
          {isSplash ? <NoPluginsEnabledSplash /> : <ConfigureDataExportBody /> }
        </Route>
        <Route exact path={`${path}/create`}>
          <EditDataExportScript scriptId='' isCreate={true} />
        </Route>
        <Route exact path={`${path}/update/:scriptId`}>
          {({ match: { params } }) => <EditDataExportScript scriptId={params.scriptId} isCreate={false} />}
        </Route>
      </Switch>
    </ConfigureDataExportPage>
  );
});
ConfigureDataExportView.displayName = 'ConfigureDataExportView';
