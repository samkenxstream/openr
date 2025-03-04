#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import datetime
from typing import List

from openr.cli.utils import utils
from openr.cli.utils.commands import OpenrCtrlCmd
from openr.OpenrCtrl import OpenrCtrl
from openr.Types import ttypes as openr_types
from openr.utils import ipnetwork, printing, serializer


class NeighborCmd(OpenrCtrlCmd):
    def _run(
        self, client: OpenrCtrl.Client, json: bool, detailed: bool, *args, **kwargs
    ) -> None:

        # Get data
        neighbors = self.fetch(client)

        # Render
        if json:
            print(serializer.serialize_json(neighbors))
        else:
            self.render(neighbors, detailed)

    def fetch(self, client: OpenrCtrl.Client) -> List[openr_types.SparkNeighbor]:
        """
        Fetch the Spark neighbors thrift structure via thrift call
        """

        return client.getNeighbors()

    def render(
        self, neighbors: List[openr_types.SparkNeighbor], detailed: bool
    ) -> None:
        """
        Render the received Spark neighbor data
        """

        if detailed:
            self._print_spark_neighbors_detailed(neighbors)
        else:
            self._print_spark_neighbors(neighbors)

    def _print_spark_neighbors_detailed(
        self, neighbors: List[openr_types.SparkNeighbor]
    ) -> None:
        """
        Construct print lines of Spark neighbors in detailed fashion

        """

        rows = []

        for neighbor in neighbors:
            v4Addr = (ipnetwork.sprint_addr(neighbor.transportAddressV4.addr),)
            v6Addr = (ipnetwork.sprint_addr(neighbor.transportAddressV6.addr),)
            helloMsgSentTimeDelta = str(
                datetime.timedelta(milliseconds=neighbor.lastHelloMsgSentTimeDelta)
            )
            handshakeMsgSentTimeDelta = str(
                datetime.timedelta(milliseconds=neighbor.lastHandshakeMsgSentTimeDelta)
            )
            heartbeatMsgSentTimeDelta = str(
                datetime.timedelta(milliseconds=neighbor.lastHeartbeatMsgSentTimeDelta)
            )

            # Top tier information for neighbor
            rows.append("")
            rows.append(
                f"Neighbor: {neighbor.nodeName}, "
                f"State: {neighbor.state}, "
                f"Last Event: {neighbor.event}"
            )
            # Neighbor attributes
            rows.append("\t[Transport Attributes]:")
            rows.append(
                f"\t\tNeighbor V4 Addr: {v4Addr}\n"
                f"\t\tNeighbor V6 Addr: {v6Addr}\n"
                f"\t\tLocal Interface: {neighbor.localIfName}\n"
                f"\t\tRemote Interface: {neighbor.remoteIfName}\n"
            )
            rows.append("\t[Other Attributes]:")
            rows.append(
                f"\t\tAreaId: {neighbor.area}\n"
                f"\t\tRtt(us): {neighbor.rttUs}\n"
                f"\t\tTCP port: {neighbor.openrCtrlThriftPort}\n"
            )
            # Spark ctrl msg info
            rows.append(f"Last SparkHelloMsg sent: {helloMsgSentTimeDelta} ago")
            rows.append(f"Last SparkHandshakeMsg sent: {handshakeMsgSentTimeDelta} ago")
            rows.append(f"Last SparkHeartbeatMsg sent: {heartbeatMsgSentTimeDelta} ago")

        print("\n".join(rows))

    def _print_spark_neighbors(
        self, neighbors: List[openr_types.SparkNeighbor]
    ) -> None:
        """
        Render neighbors without details
        """

        # print out neighbors horizontally
        rows = []
        column_labels = [
            "Neighbor",
            "State",
            "Latest Event",
            "Local Intf",
            "Remote Intf",
            "Area",
            "Rtt(us)",
        ]
        for neighbor in sorted(neighbors, key=lambda neighbor: neighbor.nodeName):
            rows.append(
                [
                    neighbor.nodeName,
                    neighbor.state,
                    neighbor.event,
                    neighbor.localIfName,
                    neighbor.remoteIfName,
                    neighbor.area,
                    neighbor.rttUs,
                ]
            )
        print("\n", printing.render_horizontal_table(rows, column_labels))


class GracefulRestartCmd(OpenrCtrlCmd):
    def _run(
        self,
        client: OpenrCtrl.Client,
        yes: bool = False,
        *args,
        **kwargs,
    ) -> None:
        question_str = "Are you sure to force sending GR msg to neighbors?"
        if not utils.yesno(question_str, yes):
            print()
            return

        client.floodRestartingMsg()
        print("Successfully forcing to send GR msgs.\n")
